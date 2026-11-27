# Market Microstructure Analytics Engine (MMAE)

An offline, strictly causal C++20 + Python research prototype for event-time L2/L3 order book reconstruction, explicit-ambiguity trade classification, queue & order-flow feature computation, and chronological short-horizon price-response / adverse-selection evaluation.

> **Scope & Disclaimer:** Independently developed educational and research prototype for offline replay only. Trade-sign inference is inherently uncertain, and statistical relationships measured on synthetic fixtures are **not** demonstrated tradable alpha. Live capital, real customer records, and production credentials are excluded.

---

## Core Capabilities

- **Causal Event-Time Book Reconstruction (C++20):** Single-pass L2 (`L2_SNAPSHOT_CLEAR`, `L2_SET`) and L3 (`L3_ADD`, `L3_MODIFY`, `L3_CANCEL`, `L3_EXECUTE`) replay with integer tick-indexed prices, bounded capacity (`max_levels`, `max_orders`), sequence-gap detection & snapshot recovery, and crossed/locked/stale book flagging.
- **Dual Book Implementations with Verified Parity:**
  - `baseline`: `std::map` + `std::list` + `std::unordered_map` reference book.
  - `optimized`: pre-allocated contiguous sorted level arrays + fixed-capacity intrusive order slot pool + zero-tombstone open-addressing hash table (zero hot-path heap allocation).
- **Trade Classification with Explicit Ambiguity:** Strictly pre-trade causal quote rule that explicitly marks midpoint prints, crossed/locked/incomplete/stale books, and active-gap trades as `AMBIGUOUS` (`trade_sign = 0`) with granular `ClassificationReason` codes (while recording a separate diagnostic `tick_rule_hint`).
- **Microstructure Features:** Spread (ticks, abs, bps), midprice, microprice & offset, L1 & Top-$K$ depth and Order Book Imbalance (`obi_l1`, `obi_k`), Cont-Kukanov-Stoikov incremental & rolling `OFI`, rolling signed flow & ambiguity ratios, L3 FIFO queue-ahead metrics, non-FIFO anomaly detection, top-of-book depletion, and cancel-to-add ratio.
- **Chronological Offline Evaluation (Python / optional Polars):** Strictly separated from feature extraction; computes short-horizon signed price response $R(h)$, Glosten/Huang-Stoll adverse selection decomposition ($s_{\text{eff}} = s_{\text{real}}(h) + \text{AS}(h)$), coverage, gap exclusions, and embargoed In-Sample / Out-of-Sample predictive statistics.

---

## Quickstart

### Prerequisites
- C++20 compiler (`clang++` or `g++`) and `cmake >= 3.20`
- `python3 >= 3.9` (runs with standard library only; optionally `pip install -r requirements.txt` for pinned `polars==1.17.1` and `pytest==8.3.4`)

### 1. Build and Run All Validation Tests
```bash
cd projects/17-market-microstructure-analytics-engine
./scripts/run_validation.sh
```

### 2. Run the Reproducible End-to-End Demo & Benchmark
```bash
./scripts/run_demo.sh
```

---

## Example Measured Output

### Hot-Path Benchmark (Actual Measured on Apple M5, Apple LLVM 21.0.0 `-O3 -std=c++20`)
From `reports/benchmark_results.json` (20,000 warm-up + 100,000 measured L3/TRADE events, `std::chrono::steady_clock`, `logical_parity_verified = true`):

| Variant | Throughput | Mean Latency | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| `baseline` | 4.59M events/sec | 189.6 ns | 167 ns | 375 ns | 1,000 ns |
| `optimized` | **7.51M events/sec (1.64×)** | **110.4 ns** | **83 ns** | **250 ns** | **792 ns** |

### Synthetic Signed-Flow Evaluation (Synthetic Fixture `seed=42`, Not Live Market Data)
From `reports/synthetic_evaluation.json` (5,243 events, 1,200 trades, 92.75% classified coverage, 7.25% explicit ambiguity, 2/2 injected gaps flagged & resolved, 500ms chronological embargo):
- **Price response $R(h)$:** `+0.290` ticks (50ms) → `+0.485` ticks (100ms) → `+0.910` ticks (250ms) → `+1.124` ticks (500ms).
- **Adverse selection identity residual $\max |s_{\text{eff}} - (s_{\text{real}} + \text{AS})|$:** `0.0` across all horizons.
- **Chronological OOS $R^2$ (vs zero baseline):** `+0.227` (50ms), `+0.318` (100ms), `+0.307` (250ms), `+0.201` (500ms).

---

## Project Layout & Documentation

- `plan.md` — Original implementation plan and milestone specification
- `docs/architecture.md` — Component diagram, causal data flow, invariants, and trade-offs
- `docs/contracts.md` — Input/output schemas (`mmae.v1`), units, validation error codes, and fixture catalog
- `docs/integration.md` — Adapter boundaries, dependency license inventory, and fixture provenance
- `docs/runbook.md` — Build, demo, CLI usage, exit codes, and operational recovery guide
- `docs/adr/0001-causal-fixed-point-engine-and-ambiguity.md` — Architecture Decision Record 0001
- `reports/acceptance.md` — Full acceptance criteria mapping and measured evidence summary
- `reports/*.json` — Raw versioned machine-readable diagnostics, parity, evaluation, and benchmark outputs
- `include/mmae/` & `src/` — C++20 order book library, causal feature engine, I/O adapters, and `mmae_cli`
- `python/mmae_analytics/` — Chronological evaluator, synthetic signed-flow generator, and parity verifier
- `tests/cpp/` & `tests/python/` — Unit, integration, causal-invariance, reproducibility, and operational failure tests
- `benchmarks/bench_engine.cpp` — Baseline vs optimized hot-path latency/throughput harness

---

## Assumptions & Known Limitations

1. **Trade-sign uncertainty:** Quote-rule inference cannot definitively classify midpoint trades or trades printed during crossed, locked, stale, or gapped book states; these are explicitly labeled `AMBIGUOUS`.
2. **Single-symbol replay instance:** Each `FeatureEngine` instance tracks one symbol; multi-symbol streams should be partitioned by symbol upstream.
3. **Synthetic vs live data:** Predictive statistics in `reports/synthetic_evaluation.json` validate the analytical pipeline on a known synthetic signed-flow process and do not imply live market profitability or account for fees, latency, or queue-priority fill risk.
4. **Optional Polars backend:** In environments without `polars==1.17.1` installed, `mmae_analytics` runs identically using Python's standard library (`backend: "python-stdlib"`); Polars acceleration is untested locally and verified in CI (`.github/workflows/ci.yml`).
