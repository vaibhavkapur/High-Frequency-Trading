# Operator & Developer Runbook

## 1. Prerequisites

- C++20 compiler (`clang++` or `g++`) and `cmake >= 3.20`
- `python3 >= 3.9` (no external packages required; optionally `pip install -r requirements.txt` for Polars `1.17.1` and `pytest 8.3.4`)

All commands below assume working directory:
```bash
cd projects/17-market-microstructure-analytics-engine
```

---

## 2. Build & Full Validation

Run the full build, CTest C++ suite, and Python end-to-end validation suite:
```bash
./scripts/run_validation.sh
```

What this verifies:
1. Clean Release C++20 build (`libmmae_core.a`, `mmae_cli`, `mmae_bench`, `mmae_tests`) with strict warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`).
2. `ctest`: hand-worked L3 exact match (both `baseline` and `optimized`), crossed/locked/stale books & sequence gaps, causal prefix invariance (no future quote leakage), bounded capacity enforcement, invalid input rejection, L3 FIFO priority rules, and baseline/optimized parity.
3. Python `unittest`: end-to-end CLI verification against `hand_worked_l3_expected.json`, edge-case gap flagging, synthetic signed-flow integration & chronological OOS evaluation, end-to-end no-future-quote-leakage test, fixed-seed SHA-256 reproducibility, and operational failure-mode checks.

---

## 3. Reproducible End-to-End Demo & Benchmark

```bash
./scripts/run_demo.sh
```

Outputs generated in `reports/`:
- `reports/hand_worked_diagnostics.json` — diagnostics for the 12-event reference fixture
- `reports/synthetic_diagnostics_baseline.json` & `reports/synthetic_diagnostics_optimized.json` — replay diagnostics for the `seed=42` synthetic signed-flow stream
- `reports/parity_report.json` — SHA-256 exact byte-parity comparison between `baseline` and `optimized`
- `reports/synthetic_evaluation.json` — chronological price response, adverse selection decomposition, coverage, gap list, and embargoed OOS predictive statistics
- `reports/benchmark_results.json` — Release-mode baseline vs optimized hot-path throughput and latency percentiles (p50/p95/p99/p99.9)

---

## 4. Running `mmae_cli` Directly

```bash
./build/mmae_cli \
  --config fixtures/synthetic_signed_flow_config.json \
  --variant optimized \
  --input <events.csv> \
  --output-features <features.csv> \
  --output-diagnostics <diagnostics.json> \
  --run-id my-replay-001 \
  --strict-fail-on-invalid
```

### Exit Codes

| Exit Code | Meaning | Recovery Action |
|---|---|---|
| `0` | Success (all events processed; under `--strict-fail-on-invalid`, zero rejected rows). | Inspect `diagnostics.json` for gaps, staleness, and ambiguity ratios. |
| `2` | Invalid CLI arguments or config JSON load/validation failure. | Check `--config` path, JSON syntax, positive `tick_size`, `stale_threshold_ns`, `flow_window_ns`, `depth_levels_k <= max_levels`, and valid `--variant` (`baseline` or `optimized`). |
| `3` | Input CSV missing, unreadable, empty, or wrong header. | Verify file path and exact header `event_ts_ns,recv_ts_ns,seq_num,symbol,event_type,side,price_ticks,qty,order_id`. |
| `4` | Output feature CSV or diagnostics JSON could not be written. | Ensure target parent directory exists and is writable. |
| `5` | `--strict-fail-on-invalid` set and one or more input rows failed validation. | Inspect `validation_errors[]` in `diagnostics.json` for line numbers and `error_code`s; remediate upstream feed/export or rerun without `--strict-fail-on-invalid` if partial replay with explicit error accounting is desired. |

---

## 5. Diagnostics Interpretation & Recovery Playbook

- **High `gap_count` or unresolved gaps (`resolved_by_snapshot: false`):**
  - Every detected gap sets `gap_active = 1` and marks trades `AMBIGUOUS` (`GAP_ACTIVE`) until an `L2_SNAPSHOT_CLEAR` resets the book.
  - Recovery: ensure upstream replay emits an `L2_SNAPSHOT_CLEAR` + full level rebuild after packet loss; rerun replay cleanly (outputs are idempotent and overwrite safely).
- **High `stale_book_events` / `ambiguous_stale`:**
  - Indicates `event_ts_ns - last_quote_ts_ns > stale_threshold_ns` (e.g., quiet periods with `HEARTBEAT` or `TRADE` prints).
  - Recovery: verify clock synchronization between quote and trade feeds and tune `stale_threshold_ns` in config JSON for the instrument's liquidity profile.
- **`capacity_errors > 0`:**
  - Bounded `max_levels` or `max_orders` was reached.
  - Recovery: increase `max_levels` / `max_orders` in config JSON and rerun replay.
- **High `fifo_anomalies`:**
  - Indicates `L3_EXECUTE` filled orders that had resting quantity ahead of them in FIFO order (e.g., pro-rata venue matching, hidden/iceberg fills, or missing upstream cancel/execute messages). Investigate upstream feed completeness before interpreting queue-position metrics.
