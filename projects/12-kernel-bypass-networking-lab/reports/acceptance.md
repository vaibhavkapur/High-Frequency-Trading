# Acceptance Evidence Report — Kernel Bypass Networking Lab

**Date:** 2026-11-27  
**Host Environment:** macOS 26.5.1 (Darwin 25.5.0), Apple M5 (`arm64`), 10 logical CPUs, 16 GB RAM ([environment_manifest.json](environment_manifest.json))  
**Verification Script:** `python3 scripts/verify_acceptance.py .` -> **`ACCEPTANCE VERIFICATION PASSED (0 errors)`** ([comparison_summary.json](comparison_summary.json))

---

## 1. Test and Validation Matrix Mapping

| Test Area | Required Scenario (`plan.md`) | Automated Test / Artifact | Observed Result |
|---|---|---|---|
| **Domain correctness** | Loss, burst load, and queue saturation | `ctest -R domain_sequence_and_ring` (`tests/test_domain_sequence_and_ring.cpp`) | **PASSED** (7/7 tests: loss, gaps, OOO fill, duplicates, reorder-window eviction, burst pacing schedule, bounded SPSC saturation & drops, warmup exclusion, exact/bucketed quantiles, deterministic fixture vs independent oracle) |
| **Integration and resilience** | Compare identical payload and CPU policy | `ctest -R integration_loopback` (`tests/test_integration_loopback.cpp`) + `scripts/run_benchmarks.sh` | **PASSED** (3/3 tests + 6 measured loopback runs comparing `posix_udp_blocking` vs `posix_udp_busy_poll` with identical seed, packet size, pacing, and CPU policy) |
| **Boundary and failure behavior** | Missing privileged hardware or driver support | `ctest -R adapters_and_failures` (`tests/test_adapters_and_failures.cpp`) + capability probes | **PASSED** (5/5 tests: `af_xdp`, `dpdk`, and `posix_udp_recvmmsg` fail explicitly with `NOT_RUN` and `UNSUPPORTED_PLATFORM`/`FEATURE_DISABLED`/`MISSING_CAPABILITY`; `strict_pinning=true` on macOS fails explicitly; zero silent fallbacks) |
| **Reproducibility** | Same versioned inputs/configuration and fixed seed | `test_protocol_and_config`, `test_integration_loopback`, `fixture_replay_result.json` | **PASSED** (identical `config_hash`, `run_id`, deterministic payload bytes, and exact oracle match on `deterministic_feed_v1.bin`) |
| **Operational review** | Missing configuration, unsupported inputs, restart/rerun | `test_protocol_and_config` (`fixtures/invalid_configs/*.json`), `test_adapters_and_failures` | **PASSED** (5/5 invalid/missing config fixtures rejected with structured error codes; zero silent success on `sendto()` failure or missing fixture) |

---

## 2. Project-Specific Acceptance Criteria (`plan.md` Section 5)

| # | Acceptance Criterion | Status | Evidence |
|---|---|---|---|
| 1 | **Report p50/p99/p99.9, throughput and loss for each actually tested path** | **PASS** | All 6 live loopback runs (`posix_udp_blocking` and `posix_udp_busy_poll` across 10k, 50k, and burst workloads) report exact `p50`, `p95`, `p99`, `p99_9`, `min`, `max`, `achieved_recv_pps`, `achieved_recv_mbps`, and `inferred_total_lost` in [comparison_summary.json](comparison_summary.json) and individual raw JSON reports. |
| 2 | **Verify sequence accounting** | **PASS** | `accounting_invariant_holds == true` and `sequence_accounting_verified == true` verified across all 6 live measured runs, the 200k-iteration domain microbenchmark, and the deterministic fixture replay (`unique_received + missing_within_highest == highest_seq` and `unique_received + inferred_total_lost == actual_packets_sent`). |
| 3 | **No simulated result presented as measured hardware performance** | **PASS** | Every report carries `is_simulated: false`. In-memory microbenchmark is explicitly labeled `DOMAIN_HOTPATH_MICROBENCH` with a disclaimer. Unavailable Linux/NIC bypass paths (`af_xdp`, `dpdk`, `posix_udp_recvmmsg`) emit `execution_status: "NOT_RUN"`. |
| 4 | **All required test scenarios pass in the supported environment, or any unavailable hardware/data dependency is explicitly marked not run** | **PASS** | 4/4 CTest suites (20/20 individual test cases) pass on macOS Apple M5. Unavailable Linux/NIC dependencies are explicitly recorded as `NOT_RUN` in [af_xdp_status.json](af_xdp_status.json), [dpdk_status.json](dpdk_status.json), and [recvmmsg_status.json](recvmmsg_status.json). |
| 5 | **README documents setup, demo, validation, assumptions, and known limitations; measured results distinguish actual observations from synthetic fixtures, estimates and targets** | **PASS** | Documented in [../README.md](../README.md), [../docs/runbook.md](../docs/runbook.md), [../docs/contracts.md](../docs/contracts.md), and [../docs/adr/0001-transport-and-clock-architecture.md](../docs/adr/0001-transport-and-clock-architecture.md). |
| 6 | **No real transactions, customer data, credentials or proprietary employer/SAAT artifacts in source, fixtures, logs or reports** | **PASS** | 100% original code and synthetic seeded payloads (`seed=20261127`); zero external data, credentials, or proprietary artifacts. |

---

## 3. Milestone Completion Summary (M1–M4)

- **M1 — Foundation and reference fixtures:** Complete (`CMakeLists.txt`, `README.md`, `docs/contracts.md`, `docs/architecture.md`, `docs/adr/0001-transport-and-clock-architecture.md`, `kbnl_sender`, `kbnl_receiver`, `fixtures/deterministic_feed_v1.bin` + independent oracle `fixtures/expected_reference_result.json`).
- **M2 — Core domain implementation:** Complete (I/O-isolated `protocol`, `ring_buffer`, `sequence_tracker`, `latency_stats`, `pacer`; feature-gated `af_xdp_transport` & `dpdk_transport`; deterministic unit tests for loss, burst load, queue saturation, and validation errors).
- **M3 — Integration and evaluation:** Complete (thread affinity & `CLOCK_MONOTONIC_RAW` metadata; identical workload/CPU policy loopback comparison; structured JSON outputs retained in `reports/`).
- **M4 — Hardening, benchmarking and delivery:** Complete (`scripts/run_demo.sh`, `scripts/run_benchmarks.sh`, `scripts/collect_env_manifest.py`, `scripts/verify_acceptance.py`, `.github/workflows/ci.yml`, `docs/runbook.md`, `docs/integration.md`, `reports/environment_manifest.json`, and this acceptance report).

---

## 4. Raw Artifacts Inventory

- [environment_manifest.json](environment_manifest.json) — Host CPU, OS, kernel, RAM, compiler, interfaces, and clock policy
- [comparison_summary.json](comparison_summary.json) — Automated verification result and summary across all paths
- [baseline_loopback_10k_blocking.json](baseline_loopback_10k_blocking.json) & [baseline_loopback_10k_busy_poll.json](baseline_loopback_10k_busy_poll.json)
- [baseline_loopback_50k_blocking.json](baseline_loopback_50k_blocking.json) & [baseline_loopback_50k_busy_poll.json](baseline_loopback_50k_busy_poll.json)
- [burst_loopback_blocking.json](burst_loopback_blocking.json) & [burst_loopback_busy_poll.json](burst_loopback_busy_poll.json)
- [hotpath_microbench.json](hotpath_microbench.json) — Domain-only in-memory hot-path microbenchmark
- [fixture_replay_result.json](fixture_replay_result.json) — Deterministic fixture verification output
- [recvmmsg_status.json](recvmmsg_status.json), [af_xdp_status.json](af_xdp_status.json), [dpdk_status.json](dpdk_status.json) — Explicit `NOT_RUN` capability probes
