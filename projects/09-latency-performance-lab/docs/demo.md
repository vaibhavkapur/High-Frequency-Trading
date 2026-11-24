# Complete Workflow Terminal Demo

Below is an actual captured terminal run on Apple M5 (macOS 26.5.1, AppleClang 21.0.0) demonstrating build, test verification, sanitizer runs, `quick_demo`, and the full reproducible `run_lab.sh` pipeline.

## 1. Build, Unit/Concurrency/Fuzz Tests, and Quick Demo

```bash
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLPL_WERROR=ON
$ cmake --build build -j
$ ctest --test-dir build --output-on-failure
Test project /Users/vaibhavkapur/Desktop/hft-projects/09-latency-performance-lab/build
    Start 1: equivalence
1/4 Test #1: equivalence ......................   Passed    0.15 sec
    Start 2: concurrency
2/4 Test #2: concurrency ......................   Passed    0.11 sec
    Start 3: harness_and_stats
3/4 Test #3: harness_and_stats ................   Passed    0.45 sec
    Start 4: matching_fuzz
4/4 Test #4: matching_fuzz ....................   Passed    0.13 sec

100% tests passed out of 4
```

```bash
$ ./build/examples/quick_demo
=== Latency Performance Lab: Quick Demo ===
| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (arm64), 10 physical / 10 logical (4P + 6E), cache line 128 B |
| Caches (L1d / L2 / L3) | 64 KiB / 6144 KiB / 0 KiB |
| OS / kernel | macOS 26.5.1 (kernel 25.5.0) |
| Compiler / version / flags | AppleClang 21.0.0.21000101 (Release, flags: `-O3 -DNDEBUG`, `-march=native`: OFF) |
| Governor / turbo | macOS DVFS (OS-managed) / OS-managed (DVFS) |
| Clock overhead (median) | 19.17 ns |

Suite          | Benchmark            | Variant                |   p50(ns) |   p99(ns) |    Mops/s |  Cyc/op |  Ins/op |   IPC |  RepCV%
----------------------------------------------------------------------------------------------------------------------------------------
demo           | seq_scan             | aos_64B                |      1.31 |      3.48 |    762.05 |    4.93 |    6.77 |  1.37 |   40.96
demo           | seq_scan             | soa_split              |      0.93 |      1.22 |   1077.67 |    2.81 |    7.21 |  2.57 |    3.42

Checksum match: YES

Suite          | Benchmark            | Variant                |   p50(ns) |   p99(ns) |    Mops/s |  Cyc/op |  Ins/op |   IPC |  RepCV%
----------------------------------------------------------------------------------------------------------------------------------------
demo_ab        | matching_engine      | baseline_std_map       |     40.51 |     50.67 |     24.68 |  155.72 |  534.43 |  3.43 |    7.25
demo_ab        | matching_engine      | pooled_flat            |     17.75 |     19.60 |     56.35 |   75.39 |  187.07 |  2.48 |    6.71

A/B checksum match: YES | Measured p50 speedup: 2.28x
```

## 2. Sanitizers (ASan + UBSan and TSan)

```bash
$ ./scripts/run_sanitizers.sh
=== [1/2] Building and testing with ASan + UBSan ===
100% tests passed out of 4
Total Test time (real) =   2.29 sec
=== [2/2] Building and testing with TSan ===
100% tests passed out of 4
Total Test time (real) =   3.35 sec
All sanitizer runs passed cleanly!
```

## 3. Full Reproducible Lab Pipeline

```bash
$ ./scripts/run_lab.sh
[1/5] Configuring and building Release...
[2/5] Running correctness, concurrency, and fuzz tests...
100% tests passed out of 4
[3/5] Running microbenchmark suite...
Wrote: results/microbenchmarks_summary.csv
Wrote: results/microbenchmarks_raw.csv
[4/5] Running integrated Matching Engine A/B benchmark...
Equivalence verified: trades=82723, traded_qty=7861622, resting=18662, best_bid=99970, best_ask=99973, checksum=4878581049783654324
A/B Summary (computed from raw samples): p50 speedup = 1.88x, p99 speedup = 2.04x
Wrote: results/matching_ab_summary.csv
Wrote: results/matching_ab_raw.csv
Wrote: results/matching_throughput_latency.csv
[5/5] Extracting kernel disassembly & generating comparison report...
Wrote disassembly to results/disasm_kernels.txt
Wrote report to results/comparison_report.md
Wrote SVG plot to results/throughput_vs_latency.svg
Done! Report written to results/comparison_report.md
```
