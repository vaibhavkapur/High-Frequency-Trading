# Architecture & Measurement Methodology

## System Overview

```text
+-------------------------------------------------------------------+
|                     Isolated Benchmark Kernels                    |
|  AoS/SoA | Order Books | Allocators | Dispatch | Branch/Sharing   |
|  SPSC Handoff (Spin / Hybrid / Blocking) | Matching Engine A/B    |
+----------------------------------+--------------------------------+
                                   |
                                   v
+-------------------------------------------------------------------+
|                  Reproducible Measurement Harness                 |
|  - Environment & hardware baseline capture (CPU/caches/flags)     |
|  - Clock overhead calibration & batching for sub-100ns kernels    |
|  - Compiler DCE barriers (do_not_optimize / clobber_memory)       |
|  - Thread affinity pinning (Linux sched_setaffinity / macOS QoS)  |
|  - Interleaved A/B execution & multi-repetition distribution      |
+-----------------+---------------------------------+---------------+
                  |                                 |
                  v                                 v
   +------------------------------+   +-----------------------------+
   |  Hardware Perf Counters      |   |  Raw & Summary CSV / JSON   |
   |  - Linux: perf_event_open(2) |   |  - environment.json         |
   |    (cycles, insns, LLC/br)   |   |  - microbenchmarks_*.csv    |
   |  - macOS: proc_pid_rusage    |   |  - matching_ab_*.csv        |
   |    (cycles, instructions)    |   |  - matching_throughput_*.csv|
   +--------------+---------------+   +--------------+--------------+
                  |                                  |
                  +----------------+-----------------+
                                   |
                                   v
                 +------------------------------------+
                 |  scripts/analyze_results.py        |
                 |  - Programmatic A/B ratios & CV%   |
                 |  - Automated comparison_report.md  |
                 |  - Throughput-vs-latency SVG plot  |
                 +------------------------------------+
```

## Core Components

1. **Measurement & Telemetry (`include/lpl/bench_harness.hpp`, `environment.hpp`, `perf_counters.hpp`, `affinity.hpp`)**
   - **Pre-measurement baseline:** Every benchmark binary collects and records CPU model, architecture, physical/logical core topology (including P/E split on Apple Silicon), L1d/L2/L3 cache sizes, cache-line size, OS/kernel version, compiler/version/flags, `-march=native` status, CPU governor, turbo state, and median `steady_clock::now()` call overhead before executing timed workloads.
   - **Dead-code elimination (DCE) protection:** Every timed kernel returns a data-dependent `uint64_t` checksum fed into `lpl::do_not_optimize()` (inline assembly read/write barrier with `"memory"` clobber). This simultaneously blocks compiler elision and acts as a runtime equivalence invariant across compared implementations.
   - **Sub-100 ns timing discipline:** Individual operations that take 0.5–30 ns cannot be timed singly because `steady_clock::now()` itself costs ~20–45 ns. Microbenchmarks therefore time batches of `ops_per_sample` operations per sample and divide by `ops_per_sample`, repeating across multiple independent repetitions (`repetitions × samples_per_rep`) to capture both per-sample distributions and cross-run variance (`Rep CV%`).
   - **Hardware counters:**
     - On **Linux**, `PerfCounterGroup` opens an in-process `perf_event_open(2)` counter group for `cycles`, `instructions`, `cache-references`, `cache-misses`, `branches`, and `branch-misses` (with `exclude_kernel=1`, `exclude_hv=1`), complemented by `scripts/run_perf_linux.sh` (`taskset`, `numactl`, `perf stat`, `perf record`, and FlameGraph generation).
     - On **macOS**, `PerfCounterGroup` reads hardware `ri_cycles` and `ri_instructions` via `proc_pid_rusage(RUSAGE_INFO_V4)` without elevated privileges. Cache-miss and branch-miss fields are explicitly reported as `n/a` (`-1`) rather than approximated or fabricated.

2. **Six Independent Microbenchmark Suites (`include/lpl/`)**
   - **AoS vs SoA (`aos_soa.hpp`):** 64-byte aligned `OrderRecordAoS` vs split column arrays `OrderRecordsSoA` across sequential bid-notional scans, single-field random lookups, and 4-field random lookups (showing when SoA wins and when AoS wins).
   - **Order-Book Data Structures (`order_book_variants.hpp`, `flat_hash_map.hpp`):** `std::map`, contiguous sorted `std::vector` (best bid at `.back()`), `std::unordered_map`, and an open-addressing `FlatHashMap` with SplitMix64 hashing and backward-shift deletion (zero tombstones).
   - **Allocators (`allocator_bench.hpp`):** Standard `new`/`delete` vs intrusive-freelist `ObjectPool<T>` vs bump-pointer `MonotonicArena` with full C++20 lifecycle enforcement (`TrackedOrder` proves `total_constructed == total_destroyed` and `live_count == 0`).
   - **Dispatch (`dispatch.hpp`):** Virtual dispatch vs function pointers vs `std::variant`/`std::visit` vs compile-time templates / static `switch`, under both monomorphic and uniformly shuffled heterogeneous event streams.
   - **Branching & False Sharing (`branch_and_sharing.hpp`):** `natural_if` vs `forced_branch` (inline-asm guarded against if-conversion) vs `branchless_mask` on sorted and unsorted data; plus 4-thread atomic counters aligned to 8 B, 64 B, and 128 B.
   - **SPSC Handoff & Wakeup (`spsc_handoff.hpp`):** 128 B cache-line-padded lock-free SPSC ring buffer comparing `busy_spin`, `hybrid_spin_yield`, and `blocking_condvar` on tight and 50 µs-paced ping-pong workloads, reporting both one-way latency distributions and `CPU/Wall` burn ratio.

3. **Integrated Matching Engine A/B Lab (`include/lpl/matching_engine.hpp`, `src/matching_engine.cpp`)**
   - Both engines implement identical price-time FIFO limit/market matching, partial fills, cancels, and in-place size-down modifies, plus a validated little-endian binary wire codec (`encode_order_stream` / `decode_order_stream`).
   - **Engine A (Baseline):** `std::map` price levels + `std::list` order queues + `std::unordered_map` order-ID index.
   - **Engine B (Optimized):** Pre-allocated `ObjectPool<IntrusiveOrder>` + intrusive doubly-linked FIFO queues + contiguous sorted price-level vectors (BBO at `.back()`) + open-addressing `FlatHashMap` order-ID index.
   - Interleaved `ABABAB...` repetitions eliminate thermal/frequency drift bias; a coordinated-omission-aware load sweep generates the throughput-vs-latency curve.

## Explicit Assumptions & Limitations

- **macOS vs Linux affinity & counters:** XNU does not expose strict userspace core pinning (`pthread_setaffinity_np`) or unprivileged LLC/branch-miss PMU counters. On macOS, affinity uses `QOS_CLASS_USER_INTERACTIVE` + `THREAD_AFFINITY_POLICY` hints and hardware counters report cycles/instructions only. Full `perf` LLC/branch-miss counters and hard `taskset`/`numactl` pinning are supported natively on Linux via `src/perf_counters.cpp`, `src/affinity.cpp`, and `scripts/run_perf_linux.sh`.
- **DVFS / Turbo:** On developer laptops (including Apple Silicon macOS), frequency scaling is OS-managed. Interleaved A/B runs, multi-repetition CV%, and explicit environment metadata mitigate and expose any resulting variance.
- **Portability of `-march=native`:** Disabled by default (`LPL_MARCH_NATIVE=OFF`) so baseline numbers reflect portable binaries; users can opt in with `-DLPL_MARCH_NATIVE=ON` and the build metadata records it explicitly.
- **Educational scope:** The matching engine and synthetic workloads are deterministic lab benchmarks, not a production exchange or trading venue integration.
