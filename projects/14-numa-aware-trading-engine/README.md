# NUMA-Aware Trading Engine

A C++20 educational and research prototype for measuring the impact of CPU affinity, first-touch allocation, and NUMA memory placement on a bounded, low-latency trading event pipeline.

## Highlights

- **Baseline vs. Lock-Free SPSC Pipeline:** Compares a reference `std::mutex` + `std::condition_variable` + `std::deque` pipeline against a bounded, cache-line-separated (`alignas(128)`) lock-free SPSC ring buffer (`SpscRingBuffer<MarketEvent>`) carrying fixed-size 64-byte `MarketEvent` messages.
- **NUMA & Thread Placement:** Supports CPU pinning (`pthread_setaffinity_np` on Linux; explicit advisory-only reporting on macOS), deferred **first-touch** page faulting by the owning worker thread, and `default`, `first_touch_local`, `local`, `remote`, and `interleaved` memory policies (`libnuma` or Linux `SYS_mbind`).
- **Deterministic Functional Equivalence:** Pure, zero-I/O `MatchingEngine` state machine verifies bit-exact equivalence (`final_state_digest`) and zero lost events across all placements.
- **Honest Single-Node Fallback:** Automatically probes host topology (`/sys/devices/system/node` on Linux, `sysctl` on macOS) and explicitly labels unavailable multi-NUMA scenarios (`UNAVAILABLE_SINGLE_NODE` / `FALLBACK_SINGLE_NODE`) rather than claiming cross-node gains on single-node hardware.
- **Observability:** High-resolution `CLOCK_MONOTONIC_RAW` latency histograms (`p50`, `p90`, `p99`, `p99.9`, `p99.99`), backpressure spin counters, Linux `perf_event_open` hardware cache counters (with graceful fallback), and portable `getrusage` page-fault / context-switch counters.

## Quickstart

```bash
cd projects/14-numa-aware-trading-engine

# Full build, test suite, fixture validation, benchmark, and acceptance verification:
./scripts/run_demo.sh
```

Or run steps individually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Print machine topology
./build/numa_trading_engine --mode topology

# Run comparison suite (baseline vs placements, 5 repetitions)
./build/numa_trading_engine --mode compare \
  --events 200000 --warmup 10000 --capacity 4096 --repetitions 5 --seed 42 \
  --output reports/raw_benchmark_results.json
```

## Measured Results (Actual Observations on Single-Node Test Host)

> **Important distinction:** The table below reports **actual measured observations** from [reports/raw_benchmark_results.json](reports/raw_benchmark_results.json) (report ID `cmp-0f1971a1849665b9`) on a **single-node UMA laptop** (Apple M5, Darwin 25.5.0 `arm64`, 10 cores: 4P + 6E, 128B cache line, 16 KiB page size, 1 NUMA node, AppleClang 21.0.0 `-O3` Release, `CLOCK_MONOTONIC_RAW`, 200,000 measured + 10,000 warm-up synthetic events, queue capacity 4096, seed 42, 5 repetitions). Multi-NUMA cross-node scenarios (`remote`, `interleaved`) and Linux hardware PMU cache counters are **not run / unavailable** on this host.

| Scenario | Status | Reps | Mean Throughput (events/s) | Median p50 (ns) | Median p99 (ns) | Median p99.9 (ns) | Lost Events | Digest Match |
|---|---|---:|---:|---:|---:|---:|---:|:---:|
| `baseline_mutex_unpinned` | `EXECUTED` | 5 | 5,555,546 | 659,959 | 780,750 | 795,125 | 0 | PASS |
| `spsc_default_unpinned` | `EXECUTED` | 5 | 10,506,703 | 383,250 | 450,625 | 458,333 | 0 | PASS |
| `spsc_local_pinned` | `FALLBACK_SINGLE_NODE` | 5 | 10,546,454 | 383,625 | 482,000 | 490,584 | 0 | PASS |
| `spsc_first_touch_local` | `FALLBACK_SINGLE_NODE` | 5 | 10,467,872 | 388,792 | 478,750 | 494,917 | 0 | PASS |
| `spsc_remote_node` | `UNAVAILABLE_SINGLE_NODE` | 0 | N/A | N/A | N/A | N/A | 0 | N/A |
| `spsc_interleaved` | `UNAVAILABLE_SINGLE_NODE` | 0 | N/A | N/A | N/A | N/A | 0 | N/A |

**Observations from actual runs:**
- Replacing `std::mutex` + `std::condition_variable` + `std::deque` with the lock-free `SpscRingBuffer` roughly **1.89x increased throughput** (~5.56M → ~10.5M events/s) and cut saturated tail latency (`p99`) from ~781 µs to ~451–482 µs while preserving exact domain digest equivalence (`0x97900201f18b3c60`) and zero lost events.
- Because the throughput benchmark blasts 200,000 events unthrottled into a 4096-slot bounded ring, measured `p50`/`p99` reflects Little's Law queueing delay (`4096 / throughput ≈ 390 µs`). On an unloaded 12-event fixture run ([reports/small_fixture_run.json](reports/small_fixture_run.json)), observed end-to-end `p50` is **709 ns** and `p99` is **958 ns**.
- Under extreme backpressure (`capacity=16`, `consumer_work_iters=60`, [reports/backpressure_scenario.json](reports/backpressure_scenario.json)), the producer recorded **1,192,878 full-queue spins**, bounded occupancy stayed `<= 16`, end-to-end `p99` was **2,500 ns**, and **0 events were lost**.
- As expected on a single-node UMA host, `spsc_default_unpinned`, `spsc_local_pinned`, and `spsc_first_touch_local` achieve nearly identical throughput (~10.5M events/s); **no cross-node NUMA gain is claimed from single-node runs**.

## Documentation & Acceptance Evidence

- **Acceptance Mapping:** [reports/acceptance.md](reports/acceptance.md)
- **Raw Benchmark & Test Artifacts:** [reports/raw_benchmark_results.json](reports/raw_benchmark_results.json), [reports/benchmark_summary.csv](reports/benchmark_summary.csv), [reports/topology.json](reports/topology.json), [reports/backpressure_scenario.json](reports/backpressure_scenario.json), [reports/small_fixture_run.json](reports/small_fixture_run.json)
- **Architecture & ADR:** [docs/architecture.md](docs/architecture.md), [docs/adr/001-lock-free-spsc-and-numa-abstraction.md](docs/adr/001-lock-free-spsc-and-numa-abstraction.md)
- **Data Contracts & Fixtures:** [docs/contracts.md](docs/contracts.md)
- **Integration & Licenses:** [docs/integration.md](docs/integration.md)
- **Operator/Developer Runbook:** [docs/runbook.md](docs/runbook.md)
- **Original Plan:** [plan.md](plan.md)

## Assumptions & Known Limitations

- **Multi-NUMA Hardware Required for Cross-Node Metrics:** True `local` vs. `remote` vs. `interleaved` memory latency comparisons require a multi-socket Linux machine (`>= 2` NUMA nodes). On single-node systems, `remote` and `interleaved` are explicitly marked `UNAVAILABLE_SINGLE_NODE` (or `FALLBACK_SINGLE_NODE` when `--allow-fallback` is passed).
- **Platform-Specific APIs:** Hard CPU core pinning (`pthread_setaffinity_np`), explicit `libnuma`/`mbind` page binding, and hardware PMU cache-miss counters (`perf_event_open`) are Linux-specific. On macOS, Mach affinity tags are advisory only (`enforced: false`), and `perf_event_open` is unavailable (portable `getrusage` page-fault and context-switch counters are collected instead).
- **Offline Prototype Scope:** Uses synthetic market events only. No live capital, real market feeds, customer records, credentials, or proprietary artifacts are included.
