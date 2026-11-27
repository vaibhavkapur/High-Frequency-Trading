# Integration Boundaries, Dependencies, and Provenance

## 1. Repository Neighbor Audit (`Trading-High-Frequency`)

Verified sibling project paths in `projects/`:
- `projects/12-kernel-bypass-networking-lab/`
- `projects/13-fpga-order-book-accelerator/`
- `projects/15-precision-time-synchronization-platform/`
- `projects/16-order-book-compression-engine/`
- `projects/17-market-microstructure-analytics-engine/`

To remain independently buildable with zero cross-project coupling, `14-numa-aware-trading-engine` exposes clean, versioned, independent adapter boundaries for composition:

| Neighbor Project | Integration Boundary (Independent Adapter) |
|---|---|
| `kernel-bypass-networking-lab` | Feed decoded fixed-size `numa_engine::MarketEvent` (64B) directly into `SpscRingBuffer<MarketEvent>::try_push()` from a DPDK/AF_XDP RX polling thread pinned on the NIC's local NUMA node. |
| `precision-time-synchronization-platform` | Replace or wrap `MonotonicClock::now_ns()` with PTP-disciplined NIC hardware timestamps (`SO_TIMESTAMPING`) without altering `SpscRingBuffer` or `MatchingEngine`. |
| `order-book-compression-engine` | Consume `ProcessedEvent` snapshots or archive `MarketEvent` CSV/binary streams using the versioned `v1.0.0` contract. |
| `market-microstructure-analytics-engine` | Ingest `reports/raw_benchmark_results.json` or replay identical `fixtures/*.csv` streams for cross-engine latency/microstructure analysis. |

## 2. Public Library Surface (`numa_engine_lib`)

Consumers linking against the CMake target `numa_engine_lib` get typed headers under `include/numa_engine/`:
- `types.hpp` — `MarketEvent`, `ProcessedEvent`, `Result<T>`, `Error`, enums
- `topology.hpp` — `detect_topology()`, `make_synthetic_topology()`
- `memory.hpp` — `NumaBuffer`, `evaluate_memory_policy()`
- `affinity.hpp` — `pin_current_thread()`
- `spsc_queue.hpp` — `SpscRingBuffer<T>`
- `matching_engine.hpp` — `MatchingEngine` (pure, zero-I/O state machine)
- `event_generator.hpp` — `generate_synthetic_events()`, `load_events_csv()`, `validate_event_stream()`
- `perf_counters.hpp` — `LatencyHistogram`, `PerfCounterCollector`
- `pipeline.hpp` — `run_pipeline()`, `run_comparison_suite()`, `load_pipeline_config_json()`

## 3. Dependency Licenses & External Tooling

| Dependency | Requirement | License | Notes |
|---|---|---|---|
| C++20 Standard Library & POSIX Threads | Required | Toolchain standard | Tested with AppleClang 21.0.0; compatible with GCC 11+ / Clang 14+ |
| CMake >= 3.20 | Required (build) | BSD-3-Clause | Generates Makefiles/Ninja and CTest targets |
| Python 3 | Optional (verification) | PSF License | Used only by `scripts/verify_acceptance.py` |
| `libnuma` (`numactl` / `libnuma-dev`) | Optional (Linux only) | LGPL-2.1 | Dynamically detected by CMake; falls back to Linux `SYS_mbind` syscall when absent |
| Google Benchmark | Optional | Apache-2.0 | Dynamically detected by CMake; builds `gbench_pipeline` when present |

## 4. Fixture Provenance & Compliance

- All source files, fixtures, and benchmark datasets in this project are **100% original and synthetically generated** (deterministic SplitMix64 streams and hand-authored CSV/JSON test vectors).
- **No live market feeds, real transactions, customer records, credentials, or proprietary employer/SAAT artifacts** exist anywhere in source, fixtures, logs, or reports.
- Scope is strictly offline educational/research prototyping; any production deployment requires separate security, operational, legal, and risk reviews.
