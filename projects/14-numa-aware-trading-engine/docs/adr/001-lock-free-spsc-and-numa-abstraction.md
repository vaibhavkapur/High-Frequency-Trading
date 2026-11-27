# ADR-001: Lock-Free SPSC Ring Buffer, Explicit NUMA Abstraction, and Single-Node Honesty

- **Status:** Accepted
- **Date:** 2026-11-27
- **Context:** `projects/14-numa-aware-trading-engine`, C++20 educational/research prototype

## Context

We need to measure the effect of CPU affinity, first-touch allocation, and NUMA memory placement (`default`, `local`, `first_touch_local`, `remote`, `interleaved`) on a low-latency trading event pipeline while remaining buildable, testable, and honest on single-node developer machines (including macOS laptops and single-socket Linux VMs).

## Decision

1. **Hot-path transport (`SpscRingBuffer<T>`):** Implement a bounded power-of-two single-producer single-consumer ring buffer backed by an RAII `NumaBuffer` (`mmap` / `libnuma`). Separate producer and consumer metadata onto distinct 128-byte-aligned cache lines (`alignas(128)`) with cached peer indices to minimize inter-core coherency traffic.
2. **Baseline contrast (`BaselinePipeline`):** Provide a like-for-like bounded `std::mutex` + `std::condition_variable` + `std::deque<MarketEvent>` pipeline processing identical 64-byte `MarketEvent` streams through the exact same `MatchingEngine`.
3. **Pure domain isolation (`MatchingEngine`):** Isolate order-book updates and rolling 64-bit FNV-1a state digest calculation from threading, timing, and memory placement so functional equivalence across placements is verified mechanically.
4. **Explicit single-node and platform labeling:** Introduce `ScenarioStatus` (`EXECUTED`, `FALLBACK_SINGLE_NODE`, `UNAVAILABLE_SINGLE_NODE`, `UNAVAILABLE_PLATFORM`, `FAILED_VALIDATION`). Never silently pretend that `remote` or `interleaved` policies ran across nodes on a 1-node host, and never claim Linux hard CPU pinning on macOS where Mach thread affinity tags are advisory only.
5. **Zero mandatory external dependencies:** Use standard C++20 + POSIX/Linux/macOS syscalls with optional CMake detection for `libnuma` and Google Benchmark so clean checkouts compile and test deterministically offline.

## Consequences

- **Positive:**
  - Deterministic cross-platform builds and CI on both Linux and macOS.
  - Zero ambiguity between real multi-NUMA measurements and single-node fallback runs.
  - Full unit testability of multi-NUMA policy validation via `make_synthetic_topology()`.
- **Trade-offs:**
  - True cross-node memory latency deltas (`local` vs. `remote` vs. `interleaved`) and hardware PMU cache-miss counters (`perf_event_open`) can only be observed when executing the binary on multi-socket Linux hardware.
  - Fixed-size 64-byte `MarketEvent` avoids heap indirection on the hot path, at the cost of fixed schema capacity (`kMaxSymbols = 64`).
