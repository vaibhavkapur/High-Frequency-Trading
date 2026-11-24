# Benchmarks

All numbers on this page come from one run of `scripts/run_benchmarks.sh`. The
raw output is checked in under
[`results/20261124T013847Z/`](../results/20261124T013847Z/): `environment.txt`,
one JSON and one text file per run in `latency/`, `microbench.json`, and the
generated [`summary.md`](../results/20261124T013847Z/summary.md) with every
table in full.

> **Read this first.** The run was made on a laptop on battery power while other
> CPU-heavy processes were running (load average 17–22 on 10 cores), with no core
> pinning (macOS has no API for it). Run-to-run variation was large: up to 2.5x
> between repetitions of the same configuration. The numbers show the *shape* of
> the results on this machine; they are not what the code would do on an
> isolated, pinned core. Re-run the script on a quiet machine before quoting
> anything.

## Environment

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency), 16 GB RAM; performance cores report L1d 128 KiB, L2 16 MiB |
| OS / kernel | macOS 26.5.1 (25F80), Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0 (clang-2100.1.1.101), `-std=c++20 -O3 -DNDEBUG` |
| Dataset / event mix / sample count | Synthetic, `me::Workload` seed 1. 2,000,000 measured commands per run, 3 runs per configuration. Baseline mix as executed: 45% accepted new orders (9% of them aggressive), 34% cancels, 4% reduces, 4% replaces, 13% rejects (mostly cancels of already-filled orders); 210,646 trades |
| Warmup / affinity / governor | 500,000 commands of warm-up (4 × target book size when larger); no affinity; no governor control; battery power; machine not idle |
| Throughput | Baseline (1 symbol, ~7,600 resting orders), `PoolBook`: **22.5 M commands/s** median of 3 runs (range 19.8–23.0). `MapBook`: 9.6 M commands/s (9.6–11.0) |
| p50 / p99 / p99.9 latency | Baseline, `PoolBook`: **42 / 84 / 208 ns** per command. `MapBook`: 83 / 250 / 500 ns. Clock granularity is 41 ns and each sample includes one ~24 ns clock read, so these are upper bounds quantized to 41 ns steps |
| Raw results and reproduction command | `results/20261124T013847Z/`; `scripts/run_benchmarks.sh` (each `latency/*.txt` ends with the exact command line) |

Source revision: the directory was not a git checkout when the run was made, so
there is no commit hash to cite. The benchmarked sources are the ones in this
tree at the time of the run.

## Method

`me_latency` (`benchmarks/latency_main.cpp`):

1. Generates the whole command stream up front (never timed).
2. **Throughput pass.** A fresh engine processes the warm-up commands, then the
   measured commands between two clock reads. Heap allocations (counting
   `operator new`) are measured over the same window. The sink only counts reports.
3. **Latency pass.** A second fresh engine replays the same stream; each measured
   command is bracketed by two `steady_clock` reads and classified by what it did
   (passive add, aggressive add, cancel, reduce, replace, reject).
4. Percentiles are nearest-rank over all samples of the run; nothing is discarded.

`me_bench` (`benchmarks/bench_book.cpp`, Google Benchmark) times batches of 1,024
identical-kind commands against a book pre-filled to a given depth and restores
the book outside the timed region. It reports a mean per command, not a
distribution.

Limits of the method:

* **Timer resolution.** `steady_clock` on this machine ticks every 41 ns and a
  read costs about 24 ns. A single command takes about as long as one tick, so
  per-command percentiles can only take values such as 0, 41/42, 83/84, 125 ns.
  Ratios between small percentiles are not meaningful; the throughput pass and
  the batch microbenchmarks are the better measure of typical cost.
* **Tail values include the operating system.** The `max` column (tens of
  microseconds up to 132 ms) is preemption and page faults on a busy,
  unpinned machine, not matching work.
* **Everything is in-process.** No network, no file I/O, no serialization
  beyond building the report struct.
* **No hardware counters.** LLC misses, branch misses and cycle counts need
  Linux `perf`; `scripts/perf_stat.sh` is provided for that and **has not been
  run**. No such numbers are claimed here.

## Results: mixed workload

Median-throughput run of 3; the bracket is the range over the 3 runs. Latency
columns are nanoseconds per command. "Capacity/book" is `max_orders_per_book`.

| Config | Book | Capacity/book | Resting orders | Levels | Throughput [min–max], M cmd/s | p50 | p95 | p99 | p99.9 | Allocs/cmd |
|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|
| baseline, 1 symbol | pool | 41,024 | 7,630 | 119 | 22.5 [19.8–23.0] | 42 | 83 | 84 | 208 | 0.000 |
| | map | | | | 9.6 [9.6–11.0] | 83 | 167 | 250 | 500 | 0.908 |
| ~780 resting orders | pool | 5,024 | 778 | 119 | 28.1 [25.2–30.4] | 41 | 42 | 84 | 250 | 0.000 |
| | map | | | | 15.1 [12.3–15.6] | 42 | 125 | 167 | 334 | 0.916 |
| ~77k resting orders | pool | 401,024 | 76,528 | 125 | 14.7 [12.8–14.7] | 42 | 166 | 208 | 292 | 0.000 |
| | map | | | | 7.7 [7.3–7.9] | 125 | 250 | 334 | 833 | 0.907 |
| ~770k resting orders | pool | 4,001,024 | 767,964 | 120 | 8.4 [8.0–8.4] | 84 | 250 | 334 | 542 | 0.000 |
| | map | | | | 4.0 [3.1–4.2] | 208 | 583 | 750 | 1,750 | 0.907 |
| ~77k orders over 7,520 levels | pool | 401,024 | 77,354 | 7,520 | 11.9 [9.6–13.0] | 83 | 209 | 292 | 542 | 0.000 |
| | map | | | | 7.8 [6.3–7.8] | 125 | 209 | 417 | 959 | 0.929 |
| 16 symbols, ~77k orders | pool | 26,024 | 76,595 | 1,955 | 17.7 [7.1–18.0] | 42 | 125 | 208 | 333 | 0.000 |
| | map | | | | 4.3 [4.2–4.8] | 250 | 750 | 1,209 | 19,875 | 0.909 |
| 256 symbols, ~78k orders | pool | 2,584 | 77,719 | 28,580 | 11.9 [11.3–12.6] | 83 | 167 | 250 | 541 | 0.000 |
| | map | | | | 7.2 [6.6–7.3] | 125 | 334 | 666 | 1,542 | 0.953 |
| random 64-bit ids, ~7.6k orders | pool | 41,024 | 7,630 | 119 | 28.8 [28.3–31.1] | 41 | 42 | 83 | 125 | 0.000 |
| | map | | | | 12.3 [11.1–12.8] | 83 | 167 | 291 | 1,125 | 0.908 |
| random 64-bit ids, ~770k orders | pool | 4,001,024 | 767,964 | 120 | 5.7 [4.8–6.1] | 125 | 333 | 459 | 1,250 | 0.000 |
| | map | | | | 3.0 [2.9–3.5] | 292 | 583 | 791 | 3,083 | 0.907 |
| 256 symbols, **over-provisioned** | pool | 200,000 | 77,719 | 28,580 | 3.5 [3.3–4.7] | 84 | 375 | 2,084 | 7,666 | 0.000 |
| | map | | | | 6.8 [6.1–7.6] | 125 | 250 | 417 | 959 | 0.953 |

Per-class percentiles (add, aggressive add, cancel, reduce, replace, reject) for
every configuration are in `summary.md`.

## Results: single operations

Mean nanoseconds per command (per fill for Sweep), median of 3 repetitions; cv is
the coefficient of variation across the repetitions.

| Operation | Resting orders | map ns | map cv | pool ns | pool cv | map / pool |
|---|---:|---:|---:|---:|---:|---:|
| Passive add | 1,000 | 34.3 | 8% | 14.8 | 4% | 2.32x |
| Passive add | 10,000 | 35.4 | 15% | 27.7 | 6% | 1.28x |
| Passive add | 100,000 | 35.8 | 7% | 61.2 | 33% | 0.59x |
| Passive add | 1,000,000 | 77.3 | 30% | 74.1 | 29% | 1.04x |
| Cancel | 1,000 | 62.4 | 28% | 20.0 | 27% | 3.12x |
| Cancel | 10,000 | 63.2 | 1% | 28.5 | 16% | 2.21x |
| Cancel | 100,000 | 103.3 | 4% | 42.5 | 5% | 2.43x |
| Cancel | 1,000,000 | 159.1 | 13% | 51.3 | 5% | 3.10x |
| Aggressive match (1 fill) | 1,000 | 25.9 | 2% | 8.9 | 29% | 2.91x |
| Aggressive match (1 fill) | 10,000 | 26.9 | 7% | 13.5 | 14% | 1.99x |
| Aggressive match (1 fill) | 100,000 | 26.9 | 1% | 11.4 | 4% | 2.36x |
| Aggressive match (1 fill) | 1,000,000 | 28.2 | 4% | 14.2 | 9% | 1.99x |
| Sweep, 8 fills per taker | 10,000 | 49.1 | 1% | 8.3 | 9% | 5.88x |
| Sweep, 64 fills per taker | 10,000 | 49.9 | 4% | 7.3 | 2% | 6.85x |
| Sweep, 512 fills per taker | 10,000 | 37.8 | 17% | 5.7 | 5% | 6.68x |

## What the measurements support

**1. `PoolBook` makes no heap allocations while processing commands.**
0 allocations in the measured window in all 30 `PoolBook` runs, against 0.91–0.95
allocations per command for `MapBook`. `tests/test_alloc.cpp` asserts the same
thing, including from a cold start, for books with fewer than 4,096 price levels
per side.

**2. The pooled book is faster on the mixed workload when it is sized sensibly.**
In the nine right-sized configurations `PoolBook` throughput was 1.5x to 4.1x
`MapBook`'s (median runs), and the ranges over the three repetitions do not
overlap in any of them. The 4.1x figure is the 16-symbol configuration, which was
the noisiest (one `PoolBook` repetition at 7.1 M/s against 18.0 for the others,
and `MapBook` outliers giving a p99.9 of 19.9 µs); 1.5x–2.3x is the range for
the other eight.

**3. The tradeoff: preallocation punishes over-provisioning.**
Same 256-symbol workload, same ~78k resting orders, only `max_orders_per_book`
changed from 2,584 to 200,000: `PoolBook` throughput fell from 11.9 to 3.5
M commands/s and p99 rose from 250 ns to 2,084 ns, making it about half as fast
as `MapBook` (6.8 M/s), whose cost does not depend on the bound. The cause was
not profiled. The likely explanation is the order-id index: its size follows
capacity, not occupancy (256 tables × 524,288 slots × 16 bytes ≈ 2 GiB), and a
multiplicative hash spreads the live entries across all of it, so nearly every
lookup is a cache and TLB miss, and first touches are page faults.

**4. Cost grows with the number of resting orders for both books.**
`PoolBook`: 28.1 → 22.5 → 14.7 → 8.4 M commands/s at roughly 0.8k / 7.6k / 77k /
770k resting orders with the level count held near 120. The working set outgrows
the caches (a 770k-order book is about 31 MB of live orders plus a 128 MiB index).

**5. Order-id distribution matters only for large books.**
With ~770k resting orders, random 64-bit ids cost `PoolBook` about a third of its
throughput against sequential ids (8.4 → 5.7 M/s). At ~7.6k orders no penalty is
visible (the random-id run was the faster of the two, which is noise).

## What they do not support

* **Passive add on deep books.** In the microbenchmark `PoolBook` is clearly
  faster at 1,000 resting orders, but at 100,000 and 1,000,000 the two books are
  within run-to-run variation (cv around 30%), and a second 5-repetition run of
  the same case gave a different ordering. Whether the sorted level vector's
  binary search loses to the tree there is **unresolved**.
* **Any claim about absolute latency.** The clock cannot resolve a single
  command, and the machine was neither idle nor pinned.
* **Before/after profiles.** Milestone M5 asks for profiling. The before/after
  here is `MapBook` (M1) against `PoolBook` (M3) under identical workloads; no
  profiler traces or hardware-counter data were collected.

## Reproduce

```bash
scripts/run_benchmarks.sh            # full run, about 5 minutes; writes results/<UTC timestamp>/
scripts/run_benchmarks.sh --quick    # smoke test, well under a minute
ME_PIN_CPU=3 scripts/run_benchmarks.sh   # Linux: pin with taskset
scripts/perf_stat.sh --live 100000   # Linux: hardware counters (not yet run)

# one configuration by hand
build-bench/me_latency --book pool --live 10000 --count 2000000 --warmup 500000
build-bench/me_bench --benchmark_filter=Cancel
```
