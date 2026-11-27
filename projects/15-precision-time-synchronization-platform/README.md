# Precision Time Synchronization Platform (`ptsp`)

An independently developed C++20 and Python research prototype for evaluating timestamp provenance, clock offset/drift estimation under faults, software vs. Linux PTP hardware timestamp capability probing, and fail-closed causal event ordering in a distributed market-data lab.

> **Scope & Limitations:** Offline, synthetic, and local-host software-clock prototype only. Live capital, real market feeds, customer records, and production credentials are excluded. Hardware PTP accuracy depends on NICs, switches, topology, and `linuxptp` configuration; **no nanosecond hardware synchronization is claimed on hosts without verified NIC PHC evidence.**

---

## Key Capabilities

1. **Event-Time vs. Receive-Time Contracts & Clock Provenance (`ptsp.v1`)**
   - Every timestamp binds `value_ns`, non-negative `uncertainty_ns`, `ClockDomain` (`TAI`, `UTC`, `MONOTONIC_RAW`, `MONOTONIC`, `EXCHANGE_VENUE`), `TimestampRole` (`EVENT_TIME`, `RECEIVE_TIME`, `TRANSMIT_TIME`), `TimestampCaptureMethod`, `ErrorSource` (`MEASURED`, `SYNTHETIC_FIXTURE`, `ESTIMATED_MODEL`), `clock_id`, and `epoch_id`.
   - Strict domain isolation: never silently mixes clock domains (e.g., `TAI` vs `UTC` fails immediately with `CLOCK_DOMAIN_MISMATCH`).
2. **Baseline vs. Robust Drift/Offset Estimators (`libptsp_core.a`)**
   - `BaselineOlsEstimator`: unweighted sliding-window OLS reference baseline.
   - `RobustSyncEstimator`: bounded zero-allocation hot-path ring buffer with restart detection, 2-sample-confirmed clock step detection, minimum-RTT & MAD outlier rejection, 4-iteration Huber IRLS regression, and conservative uncertainty decomposition (`measurement_noise + asymmetry_bound + 3*residual_std`).
3. **Timestamp Adapters & Honest Capability Reporting**
   - `SoftwareTimestampAdapter`: real POSIX `clock_getres` / `clock_gettime` measurements tagged `MEASURED`; explicitly refuses `TAI`.
   - `LinuxPtpHardwareAdapter`: probes `/dev/ptp*` and `ETHTOOL_GET_TS_INFO` on Linux; fails closed with `HARDWARE_TIMESTAMP_UNAVAILABLE` on macOS or NICs without hardware timestamping.
   - `SyntheticFixtureTimestampAdapter`: deterministic offline adapter tagged `SYNTHETIC_FIXTURE`.
4. **Skew Violation Detection & Fail-Closed Ordering**
   - `FailClosedOrderer`: bounded-capacity reorder buffer that aligns receive-time to event-time, validates skew envelopes and receive-after-event causality, detects cross-source overlapping uncertainty intervals (`AMBIGUOUS_ORDERING`), and transitions to `HALTED_FAIL_CLOSED` until explicit operator recovery.

---

## Quickstart

### Prerequisites
- C++20 compiler (`clang++` or `g++`), `cmake >= 3.20`, `make`, and `python3 >= 3.9` (standard library only; zero external C++ or PyPI dependencies).

### Build & Validate

```bash
cd projects/15-precision-time-synchronization-platform

# 1. Generate deterministic synthetic fixtures (fixed seed 20261127)
python3 scripts/generate_fixtures.py

# 2. Configure and build in Release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Run full validation (CTest + Python unittest + demo + reproducibility check + benchmarks)
python3 scripts/validate_all.py
```

### Individual CLI Commands

```bash
# Probe local host software clock and Linux PTP hardware capability
./build/ptsp_cli probe --out reports/host_probe.json

# Replay a single versioned scenario config
./build/ptsp_cli replay --config fixtures/reference_drift.cfg --out reports/manual_replay.json --audit-out reports/manual_audit.jsonl

# Run the full 6-scenario demo suite
./build/ptsp_cli demo --fixtures-dir fixtures --reports-dir reports

# Run like-for-like hot-path microbenchmarks
./build/ptsp_bench --warmup 2000 --measured 20000 --out reports/benchmark_raw.json
```

---

## Observed Results (Measured vs. Synthetic Clearly Separated)

### 1. Host Capability Probe (`MEASURED` on Darwin 25.5.0 `arm64`, see `reports/host_probe.json`)
- **`software_clock_adapter`**: `available=true`, `error_source=MEASURED`, `clock_resolution_ns=1000`, `median_read_latency_ns=0`, `p99_read_latency_ns=1000`, `measured_realtime_vs_monotonic_jitter_ns=500`.
- **`linux_ptp_hw_adapter`**: `available=false`, `hardware_timestamping_supported=false`, `status_code=HARDWARE_TIMESTAMP_UNAVAILABLE` (Linux NIC PHC hardware timestamping is **NOT RUN** on macOS and is never fabricated).

### 2. Synthetic Fault-Injection Scenarios (`SYNTHETIC_FIXTURE`, see `reports/demo_scenarios_summary.json`)

| Scenario | Run Fingerprint | Baseline OLS MAE (ns) | Robust MAE (ns) | Robust Steady Max Err (ns) | Envelope Coverage | Orderer State |
|---|---|---:|---:|---:|---:|---|
| `reference_drift` | `5c2e35aa6b220ad3` | 1.84 | 1.86 | 4 | 100.0% | `HEALTHY` (30 emitted) |
| `clock_step_and_outliers` | `5bde5dc96a3caf73` | 2,206.03 | 1.44 | 4 | 100.0% | `HEALTHY` (25 emitted) |
| `packet_asymmetry` | `a5421eb658ed8285` | 767.96 | 2.09 | 6 | 100.0% | `HEALTHY` (20 emitted) |
| `clock_restart` | `e5bbf6bfeac3e0d4` | 43,878,127.55 | 1.92 | 5 | 100.0% | `HEALTHY` (15 emitted) |
| `out_of_order_stream` | `c3ddfcf05afd2fcc` | 1.74 | 1.78 | 4 | 100.0% | `HEALTHY` (16 emitted, 4 reordered) |
| `skew_violation_fail_closed` | `a608f5988b50042d` | 1.74 | 1.78 | 4 | 100.0% | `HALTED_FAIL_CLOSED` (`CAUSAL_VIOLATION`) |

### 3. Hot-Path Microbenchmarks (`MEASURED` execution latency on `SYNTHETIC_FIXTURE` stream, Darwin `arm64`, Clang 21.0.0 `-O3 -DNDEBUG`, see `reports/benchmark_raw.json`)

| Benchmark | Mean (ns/op) | p50 (ns) | p99 (ns) | Throughput (M ops/s) |
|---|---:|---:|---:|---:|
| `baseline_ols_estimator_update` (`window=32`) | 207.90 | 208 | 250 | 4.81 |
| `robust_sync_estimator_update` (`window=32`) | 1,284.10 | 1,250 | 1,875 | 0.78 |
| `fail_closed_orderer_ingest` | 92.54 | 83 | 125 | 10.81 |

---

## Documentation & Acceptance Evidence

- [`plan.md`](plan.md) — Original project specification and milestones (M1–M4)
- [`docs/architecture.md`](docs/architecture.md) — Component diagram, data flow, invariants, and trade-offs
- [`docs/contracts.md`](docs/contracts.md) — Schemas (`ptsp.v1`), units, event-time vs. receive-time contracts, and error codes
- [`docs/integration.md`](docs/integration.md) — Sibling repository adapter boundaries, optional `linuxptp` integration, licenses, and fixture provenance
- [`docs/runbook.md`](docs/runbook.md) — Setup, demo, validation, diagnostics, and fail-closed recovery procedures
- [`docs/adr/0001-clock-domain-isolation-and-fail-closed-ordering.md`](docs/adr/0001-clock-domain-isolation-and-fail-closed-ordering.md) — Architecture Decision Record #1
- [`reports/acceptance.md`](reports/acceptance.md) — Acceptance criteria mapped directly to automated tests and raw JSON evidence

---

## Assumptions & Known Limitations

- **Constant path asymmetry floor:** Fundamental to IEEE 1588 / PTP two-way exchanges, a *static* unknown forward-vs-reverse fiber-length asymmetry that is present even at minimum RTT cannot be observed purely from `T1..T4` timestamps without external calibration; `ptsp` bounds *dynamic* excess-RTT asymmetry via `(rtt - min_rtt) / 2`.
- **Linux NIC hardware PTP not executed on macOS:** Live `/dev/ptp0` and `SO_TIMESTAMPING` capture requires a Linux host with a PTP-capable NIC and is explicitly marked **NOT RUN** in `reports/acceptance.md` and `reports/runtime_metadata.json`.
- **Single-threaded core library:** Estimator and orderer instances are single-threaded hot-path state machines; callers synchronize across threads if sharing an instance.
