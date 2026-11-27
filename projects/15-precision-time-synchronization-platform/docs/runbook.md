# Developer & Operator Runbook

## 1. Prerequisites & Clean Build

Required tools: C++20 compiler (`clang++` or `g++`), `cmake >= 3.20`, `make`, and `python3 >= 3.9` (standard library only).

```bash
cd projects/15-precision-time-synchronization-platform

# 1. Generate deterministic fixtures
python3 scripts/generate_fixtures.py

# 2. Configure and build in Release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 2. Running the Demo and Validation Suite

```bash
# Run full automated validation (fixtures, CTest, Python tests, demo, reproducibility check, benchmarks)
python3 scripts/validate_all.py

# Or run individual commands:
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p "test_*.py" -v
./build/ptsp_cli probe --out reports/host_probe.json
./build/ptsp_cli demo --fixtures-dir fixtures --reports-dir reports
./build/ptsp_cli replay --config fixtures/reference_drift.cfg --out reports/manual_replay.json --audit-out reports/manual_audit.jsonl
./build/ptsp_bench --warmup 2000 --measured 20000 --out reports/benchmark_raw.json
```

## 3. Diagnostics and Structured Outputs

- **`reports/host_probe.json`**: Shows `MEASURED` software clock resolution/read latency and explicit Linux NIC HW PTP availability (`HARDWARE_TIMESTAMP_UNAVAILABLE` when absent).
- **`reports/scenario_<name>.json`**: Shows `run_fingerprint`, `baseline_estimator` vs `robust_estimator` accuracy, `envelope_coverage_ratio`, and `orderer` counters.
- **`reports/audit_<name>.jsonl`**: Structured per-event decision log for every rejected packet, domain mismatch, skew/causal violation, or ambiguity halt.
- **`reports/benchmark_raw.json`**: Like-for-like p50/p95/p99/max latency and throughput comparing `BaselineOlsEstimator`, `RobustSyncEstimator`, and `FailClosedOrderer`.

## 4. Failure Modes and Operator Recovery Procedures

| Symptom / Error Code | Root Cause | Operator Action / Recovery |
|---|---|---|
| `HARDWARE_TIMESTAMP_UNAVAILABLE` | Running on macOS, container without PHC passthrough, or NIC without HW timestamping | Do **not** override or spoof HW timestamps. Use `SoftwareTimestampAdapter` (with its wider `MEASURED` uncertainty) for local software experiments, or move to a Linux host with a PTP-capable NIC and verify with `ethtool -T <iface>` and `ls -l /dev/ptp*`. |
| `CLOCK_DOMAIN_MISMATCH` | Mixed clock domains (e.g., `TAI` and `UTC`, or `MONOTONIC` vs `MONOTONIC_RAW`) | Inspect `audit_*.jsonl` for the offending `packet_id` and `clock_id`. Fix upstream producer configuration so all events in a stream share one declared domain, or deploy a dedicated domain-aligned converter before ingestion. |
| `SKEW_VIOLATION` / `CAUSAL_VIOLATION` (`HALTED_FAIL_CLOSED`) | Clock uncertainty exceeded `max_skew_ns`, sustained network asymmetry grew beyond budget, or aligned receive-time preceded event-time | 1) Inspect `audit_*.jsonl` and estimator `asymmetry_bound_ns` / `steps_detected`. 2) Verify sync path / grandmaster lock (`pmc` / `ptp4l` on Linux) or inspect fixture for corrupted timestamps. 3) Once sync health is verified, invoke `FailClosedOrderer::recover_and_reset_buffer("<incident-id>")` and replay from the last known good sequence watermark. |
| `AMBIGUOUS_ORDERING` (`HALTED_FAIL_CLOSED`) | Two cross-source events have overlapping `[event_ns - unc, event_ns + unc]` intervals | Expected fail-closed behavior when timestamp uncertainty exceeds inter-event spacing. Reduce clock uncertainty (better sync/NIC HW timestamping) or quarantine affected cross-venue symbols; recover explicitly via `recover_and_reset_buffer()`. |
| `BUFFER_CAPACITY_EXCEEDED` | Burst exceeded `max_buffered_events` within `reorder_window_ns` | Increase `max_buffered_events` in config if memory budget permits, or reduce upstream burst skew / `reorder_window_ns`. |
| `MISSING_CONFIG` / `SCHEMA_VERSION_MISMATCH` / `INVALID_CONFIG` | Missing file, typo in config key, or incompatible schema | Check CLI stderr for exact line/key, ensure `schema_version=ptsp.v1`, and re-run. |
