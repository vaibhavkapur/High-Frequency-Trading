# Automated Benchmark Comparison Report

> Generated programmatically by `scripts/analyze_results.py` directly from captured CSV/JSON artifacts.

## Hardware & Software Baseline

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (arm64), 10 physical / 10 logical (4P + 6E), cache line 128 B |
| Caches (L1d / L2 / L3) | 64 KiB / 6144 KiB / 0 KiB |
| OS / kernel | macOS 26.5.1 (kernel 25.5.0) |
| Compiler / version / flags | AppleClang 21.0.0.21000101 (Release, `-O3 -DNDEBUG`, `-march=native`: False) |
| Governor / turbo | macOS DVFS (OS-managed) / OS-managed (DVFS) |
| Median clock overhead | 19.17 ns |
| Captured UTC | 2026-11-24T04:57:29Z |

## Microbenchmark Results

| Suite / Workload | Variant | p50 (ns/op) | p99 (ns/op) | Mops/s | Cyc/op | Ins/op | IPC | CPU/Wall | Rep CV% | vs Baseline |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `aos_soa/sequential_scan` | `aos_64B` | 0.60 | 2.80 | 1657.40 | 2.45 | 6.76 | 2.76 | 0.99 | 39.75% | **1.00x (base)** |
| `aos_soa/sequential_scan` | `soa_split` | 0.47 | 0.53 | 2112.67 | 1.34 | 7.19 | 5.38 | 1.00 | 13.50% | **1.27x** |
| `aos_soa/random_single_field` | `aos_64B` | 0.74 | 0.87 | 1343.75 | 2.47 | 4.01 | 1.62 | 1.00 | 5.41% | **1.00x (base)** |
| `aos_soa/random_single_field` | `soa_split` | 0.53 | 0.57 | 1874.68 | 1.98 | 3.01 | 1.52 | 1.00 | 2.79% | **1.40x** |
| `aos_soa/random_multi_field` | `aos_64B` | 0.99 | 1.05 | 1011.81 | 3.96 | 9.01 | 2.28 | 1.00 | 4.08% | **1.00x (base)** |
| `aos_soa/random_multi_field` | `soa_split` | 1.87 | 2.22 | 535.35 | 8.09 | 8.01 | 0.99 | 1.00 | 0.25% | **0.53x** |
| `order_book/book_depth_64` | `std_map` | 9.82 | 18.25 | 101.78 | 45.72 | 133.32 | 2.92 | 1.00 | 14.54% | **1.00x (base)** |
| `order_book/book_depth_64` | `sorted_vector` | 7.20 | 11.57 | 138.95 | 31.67 | 75.67 | 2.39 | 1.00 | 1.80% | **1.37x** |
| `order_book/book_depth_64` | `std_unordered_map` | 16.23 | 19.21 | 61.60 | 71.01 | 223.06 | 3.14 | 1.00 | 1.15% | **0.61x** |
| `order_book/book_depth_64` | `flat_hash_map` | 27.81 | 33.34 | 35.95 | 121.96 | 384.34 | 3.15 | 1.00 | 3.86% | **0.35x** |
| `order_book/book_depth_512` | `std_map` | 14.11 | 22.03 | 70.90 | 63.84 | 166.43 | 2.61 | 1.00 | 4.70% | **1.00x (base)** |
| `order_book/book_depth_512` | `sorted_vector` | 11.04 | 15.80 | 90.54 | 48.86 | 112.99 | 2.31 | 1.00 | 1.50% | **1.28x** |
| `order_book/book_depth_512` | `std_unordered_map` | 62.07 | 67.64 | 16.11 | 268.97 | 813.59 | 3.02 | 1.00 | 0.28% | **0.23x** |
| `order_book/book_depth_512` | `flat_hash_map` | 149.35 | 156.01 | 6.70 | 641.98 | 2113.95 | 3.29 | 1.00 | 0.91% | **0.09x** |
| `allocator/order_churn` | `heap_new_delete` | 9.77 | 23.75 | 102.37 | 42.34 | 225.08 | 5.32 | 0.98 | 3.98% | **1.00x (base)** |
| `allocator/order_churn` | `object_pool` | 2.11 | 10.09 | 473.75 | 10.71 | 32.38 | 3.02 | 0.92 | 52.53% | **4.63x** |
| `allocator/order_churn` | `monotonic_arena` | 3.40 | 10.28 | 294.11 | 14.95 | 53.64 | 3.59 | 0.98 | 3.78% | **2.87x** |
| `dispatch/monomorphic` | `virtual_dispatch` | 0.81 | 0.93 | 1240.41 | 3.31 | 22.01 | 6.66 | 1.00 | 2.15% | **1.00x (base)** |
| `dispatch/monomorphic` | `function_pointer` | 0.71 | 1.84 | 1404.36 | 3.49 | 20.03 | 5.73 | 0.99 | 9.08% | **1.13x** |
| `dispatch/monomorphic` | `template_static` | 0.24 | 0.42 | 4194.30 | 1.10 | 6.76 | 6.16 | 1.00 | 16.06% | **3.38x** |
| `dispatch/heterogeneous` | `virtual_dispatch` | 4.99 | 5.79 | 200.26 | 19.79 | 22.41 | 1.13 | 0.99 | 6.41% | **1.00x (base)** |
| `dispatch/heterogeneous` | `function_pointer` | 3.78 | 4.94 | 264.39 | 16.50 | 20.37 | 1.23 | 1.00 | 0.30% | **1.32x** |
| `dispatch/heterogeneous` | `std_variant_visit` | 6.42 | 7.74 | 155.84 | 27.89 | 33.40 | 1.20 | 1.00 | 0.30% | **0.78x** |
| `dispatch/heterogeneous` | `static_switch` | 2.36 | 3.42 | 423.50 | 10.37 | 15.37 | 1.48 | 1.00 | 6.34% | **2.11x** |
| `branch/filter_sorted` | `natural_if` | 0.06 | 0.06 | 17282.70 | 0.25 | 1.26 | 4.98 | 1.01 | 0.00% | **1.00x (base)** |
| `branch/filter_sorted` | `forced_branch` | 0.23 | 0.24 | 4320.96 | 1.01 | 6.00 | 5.96 | 1.00 | 0.13% | **0.25x** |
| `branch/filter_sorted` | `branchless_mask` | 0.06 | 0.06 | 17282.70 | 0.25 | 1.26 | 4.98 | 1.00 | 0.00% | **1.00x** |
| `branch/filter_unsorted` | `natural_if` | 0.06 | 0.06 | 17282.70 | 0.25 | 1.26 | 4.99 | 1.01 | 0.01% | **1.00x (base)** |
| `branch/filter_unsorted` | `forced_branch` | 2.33 | 2.83 | 428.81 | 10.10 | 6.02 | 0.60 | 1.00 | 0.88% | **0.02x** |
| `branch/filter_unsorted` | `branchless_mask` | 0.06 | 0.06 | 17282.70 | 0.25 | 1.26 | 4.98 | 1.00 | 0.00% | **1.00x** |
| `false_sharing/4_threads_atomic` | `align_8B_packed` | 2.51 | 3.49 | 398.44 | 35.96 | 6.41 | 0.18 | 3.45 | 7.90% | **1.00x (base)** |
| `false_sharing/4_threads_atomic` | `align_64B_padded` | 0.58 | 2.17 | 1724.11 | 10.32 | 6.32 | 0.61 | 3.31 | 55.03% | **4.33x** |
| `false_sharing/4_threads_atomic` | `align_128B_padded` | 0.42 | 0.52 | 2360.87 | 7.11 | 6.28 | 0.88 | 3.89 | 0.27% | **5.92x** |
| `spsc/ping_pong_tight` | `busy_spin_spsc` | 83.00 | 83.70 | 12.05 | n/a | n/a | n/a | 1.12 | 11.62% | **1.00x (base)** |
| `spsc/ping_pong_tight` | `hybrid_spin_yield` | 125.00 | 187.50 | 8.00 | n/a | n/a | n/a | 1.98 | 7.58% | **0.66x** |
| `spsc/ping_pong_tight` | `blocking_condvar` | 1604.50 | 3458.50 | 0.62 | n/a | n/a | n/a | 1.05 | 11.06% | **0.05x** |
| `spsc/ping_pong_paced_50us` | `busy_spin_spsc` | 104.00 | 166.50 | 9.62 | n/a | n/a | n/a | 0.85 | 9.23% | **1.00x (base)** |
| `spsc/ping_pong_paced_50us` | `hybrid_spin_yield` | 146.00 | 4878.75 | 6.85 | n/a | n/a | n/a | 1.02 | 7.43% | **0.71x** |
| `spsc/ping_pong_paced_50us` | `blocking_condvar` | 1416.50 | 3107.41 | 0.71 | n/a | n/a | n/a | 0.09 | 24.18% | **0.07x** |
| `spsc/burst_handoff` | `spsc_pinned` | 40.53 | 53.03 | 24.67 | 275.25 | 282.09 | 1.02 | 1.99 | 49.33% | **1.00x (base)** |
| `spsc/burst_handoff` | `spsc_unpinned` | 44.24 | 56.45 | 22.61 | 343.16 | 347.22 | 1.01 | 1.98 | 6.74% | **0.92x** |

## Integrated Matching Engine A/B Comparison

| Variant | p50 (ns/order) | p99 (ns/order) | p99.9 (ns/order) | Throughput (Mops/s) | Cyc/op | Ins/op | IPC | Rep CV% | Checksum |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `engine_A_baseline_std_map_list` | 39.06 | 63.81 | 101.88 | 25.60 | 159.89 | 542.40 | 3.39 | 9.86% | `4878581049783654324` |
| `engine_B_pooled_flat_intrusive` | 20.81 | 31.25 | 40.38 | 48.05 | 86.59 | 194.26 | 2.24 | 7.08% | `4878581049783654324` |

- **Computed A/B Speedup (B vs A):** **1.88x** lower p50 latency (39.06 ns -> 20.81 ns), **2.04x** lower p99 latency (63.81 ns -> 31.25 ns), **2.52x** lower p99.9 latency, **1.88x** higher median throughput (25.60 -> 48.05 Mops/s).
- **Equivalence Check:** Checksums identical (`True`).

## Throughput-vs-Latency Load Sweep

| Variant | Target Rate (k ops/s) | Achieved Rate (k ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|
| `engine_A_baseline` | 500 | 500.2 | 31.2 | 62.5 | 769.7 |
| `engine_B_pooled_flat` | 500 | 500.2 | 20.8 | 36.4 | 528.7 |
| `engine_A_baseline` | 1000 | 1000.4 | 28.6 | 65.2 | 804.7 |
| `engine_B_pooled_flat` | 1000 | 1000.4 | 20.8 | 52.1 | 191.4 |
| `engine_A_baseline` | 2000 | 2000.7 | 28.6 | 62.5 | 970.2 |
| `engine_B_pooled_flat` | 2000 | 2000.8 | 18.2 | 36.4 | 604.2 |
| `engine_A_baseline` | 4000 | 4001.4 | 28.7 | 232.1 | 1259.3 |
| `engine_B_pooled_flat` | 4000 | 4001.4 | 18.2 | 28.6 | 582.0 |
| `engine_A_baseline` | 6000 | 6001.9 | 28.6 | 44.2 | 52.1 |
| `engine_B_pooled_flat` | 6000 | 6002.1 | 15.7 | 23.5 | 384.3 |
| `engine_A_baseline` | 8000 | 8002.3 | 28.7 | 974.1 | 2011.8 |
| `engine_B_pooled_flat` | 8000 | 8002.7 | 15.6 | 23.4 | 599.1 |
| `engine_A_baseline` | 12000 | 12003.0 | 28.6 | 44.2 | 53.4 |
| `engine_B_pooled_flat` | 12000 | 12003.8 | 15.7 | 23.5 | 26.1 |
| `engine_A_baseline` | unpaced (max) | 31587.2 | 28.6 | 54.7 | 411.5 |
| `engine_B_pooled_flat` | unpaced (max) | 65974.9 | 13.1 | 20.9 | 23.4 |

