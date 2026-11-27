# Acceptance Report — Market Microstructure Analytics Engine

**Date:** 2026-11-27  
**Schema Version:** `mmae.v1`  
**Environment:** Apple M5 (10 cores, 16 GB RAM), macOS (arm64-apple-darwin25.5.0), Apple LLVM 21.0.0 (`clang-2100.1.1.101`, `-O3 -std=c++20`), CMake 4.5.1, Python 3.9.6 (`python-stdlib` backend).

> **Important distinction:** All feature calculations, parity checks, and hot-path latency/throughput benchmarks are **actual measured observations** on this hardware. Price-response, adverse-selection, and OOS predictive statistics in `reports/synthetic_evaluation.json` are measured outputs on a **deterministic synthetic signed-flow fixture (`seed=42`)**, not live market data or demonstrated tradable alpha.

---

## 1. Acceptance Criteria Mapping (`plan.md` Section 5)

| Acceptance Criterion | Status | Evidence & Verification |
|---|---|---|
| **Feature calculations match hand-worked fixtures.** | **PASS** | `tests/cpp/test_mmae.cpp` (`test_hand_worked_fixture` for both `baseline` and `optimized`) and `tests/python/test_analytics.py` (`test_hand_worked_fixture_matches_expected_exactly`) match all 12 rows of `fixtures/hand_worked_l3_expected.json` (spread, midprice, microprice, OBI L1/K, CKS-OFI, midpoint ambiguity, FIFO queue ahead, non-FIFO anomaly, depletion, cancel-to-add ratio). Diagnostics recorded in `reports/hand_worked_diagnostics.json`. |
| **Report coverage and OOS predictive statistics.** | **PASS** | `reports/synthetic_evaluation.json` reports full coverage (`valid_book_ratio = 87.35%`, `classified_trade_coverage = 92.75%`, `ambiguous_trade_ratio = 7.25%`) and chronological embargoed (`embargo_ns = 500ms`, `n_embargo_excluded = 85`) OOS statistics across `50ms`, `100ms`, `250ms`, `500ms` (RMSE/MAE vs zero baseline, OOS $R^2$, Pearson $r$, Spearman IC, directional accuracy). |
| **All gap periods flagged.** | **PASS** | `tests/cpp/test_mmae.cpp` (`test_crossed_locked_stale_and_gaps`), `tests/python/test_analytics.py` (`test_edge_cases_crossed_stale_and_all_gaps_flagged`), and `reports/synthetic_evaluation.json` confirm 100% of gaps flagged in `diagnostics.gaps[]`, trades during gaps marked `AMBIGUOUS (GAP_ACTIVE)`, snapshot clear resolution tracked, and gap-contaminated horizons excluded from evaluation. |
| **All required test scenarios pass, or unavailable dependencies marked not run.** | **PASS (with explicit note)** | CTest `mmae_cpp_suite` (1/1 passed) and Python `unittest` (6/6 passed) via `./scripts/run_validation.sh`. **Explicitly not run locally:** optional `polars==1.17.1` CSV reader backend and `pytest==8.3.4` runner (not pre-installed in offline macOS Python 3.9.6 environment; verified via `python-stdlib` backend and `python3 -m unittest`; Polars & pytest are wired into `.github/workflows/ci.yml`). |
| **README documents setup, demo, validation, assumptions, limitations, and measured vs synthetic distinction.** | **PASS** | See `README.md`, `docs/architecture.md`, `docs/contracts.md`, `docs/integration.md`, `docs/runbook.md`, and `docs/adr/0001-causal-fixed-point-engine-and-ambiguity.md`. |
| **No real transactions, customer data, credentials, or proprietary employer/SAAT artifacts.** | **PASS** | All source code is original; all fixtures are synthetic/hand-authored (`docs/integration.md` §4). |

---

## 2. Test & Validation Matrix (`plan.md` Section 4)

| Test Area | Required Scenario | Automated Test / Evidence | Observed Result |
|---|---|---|---|
| **Domain correctness** | Crossed/stale books and sequence gaps | `test_crossed_locked_stale_and_gaps` (C++) & `test_edge_cases_crossed_stale_and_all_gaps_flagged` (Python) | **PASS** — Locked, crossed, stale, and both resolved/unresolved gaps detected; all affected trades explicitly marked `AMBIGUOUS` with granular `ClassificationReason`. |
| **Integration and resilience** | Known synthetic signed flow | `test_synthetic_signed_flow_integration_and_oos_evaluation` & `reports/synthetic_evaluation.json` | **PASS** — 5,243 events, 1,200 trades (87 ambiguous), 2 injected gaps flagged; positive price response $R(h)$ (`+0.290` to `+1.124` ticks) and exact adverse-selection identity residual `0.0`. |
| **Boundary and failure behavior** | No future quote used in feature calculation | `test_no_future_quote_leakage` (C++) & `test_no_future_quote_leakage_end_to_end` (Python) | **PASS** — Mutating future quotes/trades produces bit-for-bit identical causal feature rows on the entire prefix. Bounded `max_levels`/`max_orders` overflow returns explicit `BOOK_CAPACITY_*_EXCEEDED`. |
| **Reproducibility** | Same versioned inputs/configuration and fixed seed | `test_reproducibility_fixed_seed_identical_hashes` & `reports/parity_report.json` | **PASS** — Independent runs produce identical SHA-256 (`181463891f6270ad5a04bd4306a94ef1fe276c94891a9efe18268889ef7d8d4c`) across baseline and optimized variants. |
| **Operational review** | Missing config, unsupported inputs, invalid rows, rerun | `test_operational_failure_modes_no_silent_success` (Python) & `docs/runbook.md` | **PASS** — Non-zero exit codes (`2`, `3`, `5`), structured error diagnostics in `diagnostics.json`, zero silent successes. |

---

## 3. Measured Summary Results

### 3.1 Hot-Path Benchmark (`reports/benchmark_results.json` — Actual Measured)

Workload: 120,000 deterministic L3 + TRADE events (`seed=20261127`), 20,000 warm-up + 100,000 measured events, `std::chrono::steady_clock`, Release `-O3 -std=c++20`, `logical_parity_verified = true`:

| Engine Variant | Throughput (events/sec) | Mean (ns) | p50 (ns) | p95 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|---:|
| `baseline` (`std::map` + `std::list`) | 4,589,226 | 189.6 | 167.0 | 375.0 | 1,000.0 | 1,833.0 |
| `optimized` (flat arrays + slot pool) | **7,510,045** | **110.4** | **83.0** | **250.0** | **792.0** | **1,459.0** |
| **Speedup (`optimized / baseline`)** | **1.64×** | — | — | — | — | — |

### 3.2 Synthetic Signed-Flow Evaluation (`reports/synthetic_evaluation.json` — Synthetic Fixture)

Input: 5,243 events (`seed=42`, 1,200 trades: 1,113 classified, 87 ambiguous [7.25%], 2 injected gaps flagged & resolved):

| Horizon | Eval Trades | Mean Price Response $R(h)$ (ticks) | Eff. Half-Spread (ticks) | Realized Half-Spread (ticks) | Adverse Selection $\text{AS}(h)$ (ticks) | Identity Residual | OOS $R^2$ vs Zero | OOS Pearson $r$ | OOS Spearman IC |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `50ms` | 1,107 | +0.290 | 0.995 | +0.704 | +0.290 | `0.0` | +0.227 | +0.477 | +0.456 |
| `100ms` | 1,100 | +0.485 | 0.995 | +0.510 | +0.485 | `0.0` | +0.318 | +0.566 | +0.556 |
| `250ms` | 1,082 | +0.910 | 0.995 | +0.085 | +0.910 | `0.0` | +0.307 | +0.561 | +0.584 |
| `500ms` | 1,047 | +1.124 | 0.995 | -0.129 | +1.124 | `0.0` | +0.201 | +0.479 | +0.515 |

---

## 4. Untested / Out-of-Scope Items

- **Optional Polars backend (`polars==1.17.1`):** Code path implemented in `python/mmae_analytics/evaluator.py`, not executed in this offline macOS environment because `polars` is not installed locally (`polars_installed: false` recorded in `reports/synthetic_evaluation.json`). Configured for CI installation in `.github/workflows/ci.yml`.
- **Live venue feeds / production execution / real capital:** Explicitly out of scope per `plan.md`.
