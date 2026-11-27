# Acceptance Verification Report — NUMA-Aware Trading Engine

**Report Date:** 2026-11-27  
**Schema Version:** `1.0.0`  
**Benchmark Report ID:** `cmp-0f1971a1849665b9`  
**Verification Environment:**
- **OS / Arch:** Darwin 25.5.0 (`arm64`), Apple M5 (10 logical CPUs: 4 performance + 6 efficiency, 128B cache line, 16,384B page size, 16 GiB RAM)
- **NUMA Topology:** 1 NUMA node (`is_multi_numa: false`) — captured in [topology.json](topology.json)
- **Compiler / Build:** AppleClang 21.0.0.21000101, C++20, `Release` (`-O3`), zero compiler warnings
- **Clock Source:** `CLOCK_MONOTONIC_RAW`

---

## 1. Project-Specific Acceptance Criteria Mapping

| # | Acceptance Criterion (from `plan.md`) | Status | Evidence |
|---|---|---|---|
| 1 | **Report throughput and p99/p99.9 per placement with repeated runs.** | **PASS** | [raw_benchmark_results.json](raw_benchmark_results.json) & [benchmark_summary.csv](benchmark_summary.csv) report 5 repeated runs (`seed=42`, `200,000` measured + `10,000` warm-up events, `capacity=4096`) with `mean/min/max_throughput_eps`, `median_p50_ns`, `median_p99_ns`, `median_p99_9_ns`, and full per-run latency distributions (`min/p50/p90/p99/p99.9/p99.99/max/mean/stddev`). Verified automatically by `scripts/verify_acceptance.py`. |
| 2 | **No lost events.** | **PASS** | `zero_lost_events: true` across all 20 executed benchmark repetitions (4,200,000 total events streamed with `0` lost), `test_domain_equivalence`, `test_backpressure` (`capacity=16`, `1,192,878` full-queue spins, `0` lost), and [backpressure_scenario.json](backpressure_scenario.json). |
| 3 | **Label unavailable NUMA scenarios.** | **PASS** | On this 1-node host, `spsc_remote_node` and `spsc_interleaved` are labeled `UNAVAILABLE_SINGLE_NODE` with `repetitions: 0` in [raw_benchmark_results.json](raw_benchmark_results.json); `spsc_local_pinned` and `spsc_first_touch_local` are labeled `FALLBACK_SINGLE_NODE` with `numa_policy_enforced: false` and `producer_affinity.enforced: false`. Enforced by `test_numa_fallback_and_topology` and `scripts/verify_acceptance.py`. |
| 4 | **All required test scenarios pass in the supported environment, or any unavailable hardware/data dependency is explicitly marked not run.** | **PASS** | All 4 CTest targets pass (`100% tests passed out of 4`). Hardware-dependent capabilities unavailable on macOS single-node hardware are explicitly marked **NOT RUN (hardware/OS unavailable)** in Section 3 below. |
| 5 | **README documents setup, demo, validation, assumptions, and known limitations; measured results distinguish actual observations from synthetic fixtures, estimates and targets.** | **PASS** | Verified in [README.md](../README.md), [docs/runbook.md](../docs/runbook.md), and [docs/contracts.md](../docs/contracts.md). |
| 6 | **No real transactions, customer data, credentials or proprietary employer/SAAT artifacts in source, fixtures, logs or reports.** | **PASS** | All events and fixtures are synthetic (SplitMix64 PRNG and hand-authored unit test vectors); provenance documented in [docs/integration.md](../docs/integration.md). |

---

## 2. Test & Validation Matrix Evidence

| Test Area | Required Scenario | Automated Verification | Result | Raw Artifact / Command |
|---|---|---|---|---|
| **Domain correctness** | Functional equivalence across placements | `test_domain_equivalence` + `test_contracts_and_fixtures` + `scripts/verify_acceptance.py` | **PASS** | Exact digest `0x97900201f18b3c60` across baseline, default SPSC, local SPSC, and first-touch SPSC; small fixture matches hand-traced [expected_small_output.json](../fixtures/expected_small_output.json) (`digest=0xe355777ea5227f80`, [small_fixture_run.json](small_fixture_run.json)). |
| **Integration & resilience** | Producer/consumer backpressure | `test_backpressure` | **PASS** | Bounded `capacity=16` with `consumer_work_iters=60` recorded `1,192,878` full-queue spins, `max_observed_occupancy=16`, `0` lost events, strict FIFO digest match ([backpressure_scenario.json](backpressure_scenario.json)). |
| **Boundary & failure behavior** | Single-node machine fallback | `test_numa_fallback_and_topology` | **PASS** | Strict mode rejects `remote` / `interleaved` with `UNAVAILABLE_SINGLE_NODE`; fallback mode explicitly labels `FALLBACK_SINGLE_NODE`; strict macOS pinning rejects with `UNAVAILABLE_PLATFORM_API`; synthetic 2-node topology validates `EXECUTED` path. |
| **Reproducibility** | Versioned inputs/config & fixed seed | `test_domain_equivalence` (`test_reproducibility_with_fixed_seed`) | **PASS** | Identical SplitMix64 streams, deterministic `report_id` (`cmp-0f1971a1849665b9`), identical `final_state_digest` across repeated runs. |
| **Operational review** | Missing config, unsupported inputs, rerun diagnostics | `test_contracts_and_fixtures` + CLI fixture checks | **PASS** | Missing files (`FILE_NOT_FOUND`), missing JSON keys (`INVALID_CONFIG`), invalid prices (`INVALID_PRICE` with line number), non-monotonic seq (`NON_MONOTONIC_SEQUENCE`), unknown types (`INVALID_EVENT_TYPE`), non-power-of-two capacity (`INVALID_QUEUE_CAPACITY`) all return non-zero exit codes and structured diagnostics with no silent success. |

---

## 3. Explicitly Marked Unavailable / Not Run Hardware Capabilities

The following capabilities are implemented in source (`src/topology.cpp`, `src/memory.cpp`, `src/affinity.cpp`, `src/perf_counters.cpp`, `benchmarks/gbench_pipeline.cpp`) and validated via unit tests on synthetic topologies where possible, but could **not** be executed natively on the single-node macOS ARM64 verification host:

| Capability | Status on Verification Host | Reason | How to Run on Target Hardware |
|---|---|---|---|
| Cross-node `remote` vs `local` NUMA latency measurement | **NOT RUN (hardware unavailable)** | Host has 1 NUMA node (Apple Silicon UMA) | Run `./scripts/run_demo.sh` on a `>= 2`-socket Linux server (`numactl --hardware` showing `>= 2` nodes). |
| Multi-node `interleaved` memory placement measurement | **NOT RUN (hardware unavailable)** | Host has 1 NUMA node | Run on a `>= 2`-socket Linux server with `libnuma-dev` or Linux `SYS_mbind`. |
| Linux hard CPU core pinning (`pthread_setaffinity_np`) | **NOT RUN (OS unavailable)** | macOS exposes advisory Mach affinity tags only (`enforced: false`) | Run on Linux (`Ubuntu` CI job verifies Linux build & `pthread_setaffinity_np`). |
| Hardware PMU cache-miss counters (`perf_event_open`) | **NOT RUN (OS unavailable)** | `perf_event_open` is Linux-only (`hw_counters_available: false`; `getrusage` page faults & context switches verified instead) | Run on bare-metal Linux with `/proc/sys/kernel/perf_event_paranoid <= 1` or `CAP_PERFMON`. |
| Optional Google Benchmark binary (`gbench_pipeline`) | **NOT RUN (optional dependency not installed)** | `find_package(benchmark QUIET)` not found on host; native `bench_placements` harness used instead | Install `libbenchmark-dev` / `brew install google-benchmark` and reconfigure CMake. |

---

## 4. Milestone Completion Summary

- **M1 — Foundation and reference fixtures:** Complete (`CMakeLists.txt`, `README.md`, `fixtures/*`, `expected_small_output.json`, `docs/contracts.md`, `docs/architecture.md`, `docs/adr/001-lock-free-spsc-and-numa-abstraction.md`, baseline pipeline).
- **M2 — Core domain implementation:** Complete (`SpscRingBuffer`, `NumaBuffer` first-touch & NUMA policies, `affinity.hpp`, pure `MatchingEngine`, `test_domain_equivalence`, structured `Error`/`Result<T>` validation).
- **M3 — Integration and evaluation:** Complete (baseline vs. placement comparison suite, backpressure integration test & report, structured JSON/CSV output with deterministic IDs, raw artifacts retained in `reports/`).
- **M4 — Hardening, benchmarking and delivery:** Complete (topology probe, latency histograms, cache-counter collector + `getrusage`, single-node fallback regression tests, `.github/workflows/ci.yml`, `docs/runbook.md`, `docs/integration.md`, and this acceptance report).
