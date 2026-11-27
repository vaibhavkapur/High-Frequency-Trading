# Acceptance Evidence Report — Precision Time Synchronization Platform (`ptsp`)

**Schema version:** `ptsp.v1`  
**Validation environment:** Darwin 25.5.0 (`arm64`), Apple Clang 21.0.0 (`-std=c++20 -O3 -DNDEBUG`), CMake 4.5.1, Python 3.9.6 (stdlib only)  
**Raw artifacts:** `reports/demo_scenarios_summary.json`, `reports/host_probe.json`, `reports/benchmark_raw.json`, `reports/runtime_metadata.json`, `reports/audit_*.jsonl`

---

## 1. Project-Specific Acceptance Criteria

| Acceptance Criterion | Status | Evidence & Observed Results |
|---|---|---|
| Recover injected offset within documented error envelope | **PASS** (Synthetic Fixtures) | Across all 6 synthetic fixtures (`SYNTHETIC_FIXTURE`), `RobustSyncEstimator` achieved steady-state max abs offset error `<= 6 ns` (vs configured envelopes of `100–150 ns`), drift MAE `<= 0.26 ppb` (vs `15–30 ppb`), and `100.0%` (`1.0000`) uncertainty-envelope coverage. Verified by `test_estimator_domain` and `reports/demo_scenarios_summary.json`. |
| Never silently mix clock domains | **PASS** | Enforced by `require_same_domain()`, `RobustSyncEstimator` domain lock, `FailClosedOrderer` domain checks, and `SyntheticFixtureTimestampAdapter` domain check. Verified by `test_contracts_and_validation`, `test_estimator_domain` (TAI vs UTC sync sample rejected with `CLOCK_DOMAIN_MISMATCH`), `test_integration_and_ordering` (UTC packet in TAI stream rejected with `CLOCK_DOMAIN_MISMATCH`), and `test_adapters_and_hw_failure`. |
| Report measured or synthetic error separately | **PASS** | Every report and provenance record carries an explicit `error_source` (`MEASURED`, `SYNTHETIC_FIXTURE`, or `ESTIMATED_MODEL`). `reports/host_probe.json` reports `MEASURED` host software clock resolution (`1000 ns`), p99 read latency (`1000 ns`), and realtime-vs-monotonic jitter (`500 ns`), while `reports/demo_scenarios_summary.json` is tagged `SYNTHETIC_FIXTURE`. Verified by `test_adapters_and_hw_failure` and `tests/test_python_pipeline.py`. |
| All required test scenarios pass, or unavailable hardware/data dependencies explicitly marked not run | **PASS** (Linux NIC HW PTP explicitly marked **NOT RUN**) | All 4/4 C++ CTest suites and 4/4 Python unittest end-to-end tests passed (`python3 scripts/validate_all.py`). Live Linux NIC hardware PTP (`SO_TIMESTAMPING` / `/dev/ptp0` + `linuxptp`) is **NOT RUN** on this macOS `arm64` host; the unavailable-hardware failure path was executed and verified to fail closed (`HARDWARE_TIMESTAMP_UNAVAILABLE`) in `test_adapters_and_hw_failure` and `reports/host_probe.json`. |
| README documents setup, demo, validation, assumptions, limitations; distinguishes observations from synthetic fixtures, estimates and targets | **PASS** | Documented in `README.md`, `docs/architecture.md`, `docs/contracts.md`, `docs/integration.md`, and `docs/runbook.md`. |
| No real transactions, customer data, credentials or proprietary employer/SAAT artifacts | **PASS** | All fixtures are generated deterministically by `scripts/generate_fixtures.py` with synthetic symbols (`SYNTH-EQ1`, `SYNTH-EQ2`, `SYNTH-FX1`) and synthetic epoch `1700000000000000000 ns`. |

---

## 2. Test and Validation Matrix Mapping

| Test Area | Required Scenario | Automated Verification | Observed Outcome |
|---|---|---|---|
| Domain correctness | Clock step, drift, packet asymmetry, restart | `tests/test_estimator_domain.cpp` + `reports/scenario_*.json` | **PASS** — `reference_drift` MAE `1.86 ns` (drift MAE `0.14 ppb`); `clock_step_and_outliers` 1 step detected, 5 outliers rejected, Robust MAE `1.44 ns` vs Baseline `2,206.03 ns`; `packet_asymmetry` 7 bursts rejected, Robust MAE `2.09 ns` vs Baseline `767.96 ns`; `clock_restart` 1 restart detected, Robust MAE `1.92 ns` vs Baseline `43.88 ms`. |
| Integration & resilience | Out-of-order timestamps + skew violation fail-closed ordering | `tests/test_integration_and_ordering.cpp` + `reports/scenario_out_of_order_stream.json` + `reports/scenario_skew_violation_fail_closed.json` | **PASS** — 16/16 out-of-order packets emitted monotonically with 4 inversions reordered (`HEALTHY`); injected `-25,000 ns` causal skew violation halted orderer (`HALTED_FAIL_CLOSED`, `CAUSAL_VIOLATION`) and rejected all 4 subsequent packets fail-closed; cross-source overlapping uncertainty intervals halted with `AMBIGUOUS_ORDERING` and recovered explicitly via `recover_and_reset_buffer()`. |
| Boundary & failure behavior | Unavailable hardware timestamp capability | `tests/test_adapters_and_hw_failure.cpp` + `tests/test_python_pipeline.py` + `reports/host_probe.json` | **PASS** — `LinuxPtpHardwareAdapter` reports `available=false`, `hardware_timestamping_supported=false`, `status_code=HARDWARE_TIMESTAMP_UNAVAILABLE` and fails `capture_now()` explicitly without fabricating timestamps. |
| Reproducibility | Same versioned inputs/config and fixed seed | `scripts/validate_all.py` (step 5) + `tests/test_python_pipeline.py` | **PASS** — Repeated `ptsp_cli demo` and `ptsp_cli replay` executions produced bit-for-bit identical JSON reports and `run_fingerprint` values (`5c2e35aa6b220ad3`, `5bde5dc96a3caf73`, `a5421eb658ed8285`, `e5bbf6bfeac3e0d4`, `c3ddfcf05afd2fcc`, `a608f5988b50042d`). |
| Operational review | Missing config, invalid schema/keys, bounded capacity, restart/recovery | `tests/test_contracts_and_validation.cpp` + `tests/test_integration_and_ordering.cpp` + `docs/runbook.md` | **PASS** — Explicit `MISSING_CONFIG`, `SCHEMA_VERSION_MISMATCH`, `INVALID_CONFIG`, `BUFFER_CAPACITY_EXCEEDED`, non-zero CLI exit codes, and documented operator recovery in `docs/runbook.md`. |

---

## 3. Baseline vs Optimized Comparison (Raw Evidence)

### 3.1 Accuracy (`SYNTHETIC_FIXTURE`, from `reports/demo_scenarios_summary.json`)

| Scenario | Fingerprint | Baseline OLS MAE (ns) | Robust Estimator MAE (ns) | Robust Steady-State Max Error (ns) | Envelope Coverage | Orderer Final State |
|---|---|---:|---:|---:|---:|---|
| `reference_drift` | `5c2e35aa6b220ad3` | 1.84 | 1.86 | 4 | 100.0% | `HEALTHY` (30 emitted) |
| `clock_step_and_outliers` | `5bde5dc96a3caf73` | 2,206.03 | 1.44 | 4 | 100.0% | `HEALTHY` (25 emitted) |
| `packet_asymmetry` | `a5421eb658ed8285` | 767.96 | 2.09 | 6 | 100.0% | `HEALTHY` (20 emitted) |
| `clock_restart` | `e5bbf6bfeac3e0d4` | 43,878,127.55 | 1.92 | 5 | 100.0% | `HEALTHY` (15 emitted) |
| `out_of_order_stream` | `c3ddfcf05afd2fcc` | 1.74 | 1.78 | 4 | 100.0% | `HEALTHY` (16 emitted, 4 reordered) |
| `skew_violation_fail_closed` | `a608f5988b50042d` | 1.74 | 1.78 | 4 | 100.0% | `HALTED_FAIL_CLOSED` (`CAUSAL_VIOLATION`) |

### 3.2 Microbenchmark Latency (`MEASURED` on Darwin `arm64`, Clang 21.0.0 `-O3 -DNDEBUG`, from `reports/benchmark_raw.json`)

Clock source: `std::chrono::steady_clock (CLOCK_MONOTONIC)`. Warm-up: `2,000` ops. Measured: `20,000` ops on identical deterministic streams (`window_size=32`).

| Component | Mean (ns/op) | p50 (ns) | p95 (ns) | p99 (ns) | Max (ns) | Throughput (M ops/s) |
|---|---:|---:|---:|---:|---:|---:|
| `baseline_ols_estimator_update` | 207.90 | 208 | 250 | 250 | 625 | 4.81 |
| `robust_sync_estimator_update` | 1,284.10 | 1,250 | 1,750 | 1,875 | 34,250 | 0.78 |
| `fail_closed_orderer_ingest` | 92.54 | 83 | 125 | 125 | 459 | 10.81 |

---

## 4. Explicitly Untested / Unavailable Capabilities

- **Live Linux NIC Hardware PTP (`SO_TIMESTAMPING` / `/dev/ptp0` + `linuxptp`)**: **NOT RUN** on the validation host (`Darwin 25.5.0 arm64`). Code paths are conditionally compiled under `#if defined(__linux__)` and fail closed with `HARDWARE_TIMESTAMP_UNAVAILABLE` whenever PHC/NIC support is absent. No sub-microsecond hardware synchronization claim is made without Linux NIC measurement evidence.
