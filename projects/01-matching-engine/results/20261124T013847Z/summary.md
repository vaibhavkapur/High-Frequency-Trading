# Benchmark summary: 20261124T013847Z

## Environment

```
date_utc: 2026-11-24T01:38:47Z
source_revision: not a git checkout
source_dirty: 0 modified files
os: Darwin 25.5.0 arm64
os_version: macOS 26.5.1 (25F80)
cpu: Apple M5
cores_physical: 10
cores_performance: 4
cores_efficiency: 6
memory_bytes: 17179869184
l1d_bytes: 131072
l2_bytes: 16777216
power: Now drawing from 'Battery Power'
affinity: none (macOS has no CPU pinning API; scheduler picks the core)
governor: n/a (macOS)
load_average: 16.72 13.80 8.56
compiler: Apple clang version 21.0.0 (clang-2100.1.1.101)
cmake: cmake version 4.5.1
build_type: Release
cxx_flags: -std=c++20 -O3 -DNDEBUG
measured_commands_per_run: 2000000
repetitions: 3
```

## Mixed workload (me_latency)

Clock granularity 41 ns, clock read cost about 24 ns. Latencies are per command and include one clock read; values below the granularity cannot be resolved. Throughput is measured in a separate pass without per-command clock reads. Each cell is the median-throughput run of the repetitions; the throughput range across repetitions is shown in brackets.

| Config | Book | Capacity/book | Resting orders | Levels (bid+ask) | Throughput [min-max] | p50 | p95 | p99 | p99.9 | max | Allocs/cmd |
|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| baseline_live10k | pool | 41,024 | 7,630 | 119 | 22.5 M/s [19.8-23.0] | 42 | 83 | 84 | 208 | 51,958 | 0.000 |
| baseline_live10k | map | 41,024 | 7,630 | 119 | 9.6 M/s [9.6-11.0] | 83 | 167 | 250 | 500 | 92,334 | 0.908 |
| depth_live1k | pool | 5,024 | 778 | 119 | 28.1 M/s [25.2-30.4] | 41 | 42 | 84 | 250 | 32,500 | 0.000 |
| depth_live1k | map | 5,024 | 778 | 119 | 15.1 M/s [12.3-15.6] | 42 | 125 | 167 | 334 | 64,541 | 0.916 |
| depth_live100k | pool | 401,024 | 76,528 | 125 | 14.7 M/s [12.8-14.7] | 42 | 166 | 208 | 292 | 47,208 | 0.000 |
| depth_live100k | map | 401,024 | 76,528 | 125 | 7.7 M/s [7.3-7.9] | 125 | 250 | 334 | 833 | 113,375 | 0.907 |
| depth_live1m | pool | 4,001,024 | 767,964 | 120 | 8.4 M/s [8.0-8.4] | 84 | 250 | 334 | 542 | 1,449,500 | 0.000 |
| depth_live1m | map | 4,001,024 | 767,964 | 120 | 4.0 M/s [3.1-4.2] | 208 | 583 | 750 | 1750 | 1,612,583 | 0.907 |
| levels_4096_live100k | pool | 401,024 | 77,354 | 7,520 | 11.9 M/s [9.6-13.0] | 83 | 209 | 292 | 542 | 1,059,875 | 0.000 |
| levels_4096_live100k | map | 401,024 | 77,354 | 7,520 | 7.8 M/s [6.3-7.8] | 125 | 209 | 417 | 959 | 19,478,250 | 0.929 |
| symbols16_live100k | pool | 26,024 | 76,595 | 1,955 | 17.7 M/s [7.1-18.0] | 42 | 125 | 208 | 333 | 106,791 | 0.000 |
| symbols16_live100k | map | 26,024 | 76,595 | 1,955 | 4.3 M/s [4.2-4.8] | 250 | 750 | 1209 | 19875 | 131,992,333 | 0.909 |
| symbols256_live100k | pool | 2,584 | 77,719 | 28,580 | 11.9 M/s [11.3-12.6] | 83 | 167 | 250 | 541 | 32,875 | 0.000 |
| symbols256_live100k | map | 2,584 | 77,719 | 28,580 | 7.2 M/s [6.6-7.3] | 125 | 334 | 666 | 1542 | 3,269,667 | 0.953 |
| randomids_live10k | pool | 41,024 | 7,630 | 119 | 28.8 M/s [28.3-31.1] | 41 | 42 | 83 | 125 | 17,375 | 0.000 |
| randomids_live10k | map | 41,024 | 7,630 | 119 | 12.3 M/s [11.1-12.8] | 83 | 167 | 291 | 1125 | 587,416 | 0.908 |
| randomids_live1m | pool | 4,001,024 | 767,964 | 120 | 5.7 M/s [4.8-6.1] | 125 | 333 | 459 | 1250 | 487,167 | 0.000 |
| randomids_live1m | map | 4,001,024 | 767,964 | 120 | 3.0 M/s [2.9-3.5] | 292 | 583 | 791 | 3083 | 111,250 | 0.907 |
| symbols256_live100k_oversized | pool | 200,000 | 77,719 | 28,580 | 3.5 M/s [3.3-4.7] | 84 | 375 | 2084 | 7666 | 4,653,292 | 0.000 |
| symbols256_live100k_oversized | map | 200,000 | 77,719 | 28,580 | 6.8 M/s [6.1-7.6] | 125 | 250 | 417 | 959 | 69,709 | 0.953 |

### Latency by command class (ns), median-throughput run

| Config | Book | Class | Count | p50 | p95 | p99 | p99.9 | max | mean |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| baseline_live10k | pool | add_passive | 821,896 | 41 | 42 | 83 | 125 | 29,459 | 32.0 |
| baseline_live10k | pool | add_aggressive | 77,780 | 41 | 42 | 84 | 167 | 32,125 | 31.3 |
| baseline_live10k | pool | cancel | 686,762 | 42 | 83 | 125 | 209 | 51,958 | 42.0 |
| baseline_live10k | pool | reduce | 75,471 | 42 | 84 | 125 | 209 | 16,750 | 45.3 |
| baseline_live10k | pool | replace | 76,540 | 83 | 125 | 167 | 292 | 10,167 | 68.4 |
| baseline_live10k | pool | reject | 261,551 | 41 | 42 | 42 | 84 | 34,208 | 24.3 |
| baseline_live10k | map | add_passive | 821,896 | 84 | 125 | 167 | 250 | 92,334 | 94.9 |
| baseline_live10k | map | add_aggressive | 77,780 | 83 | 250 | 334 | 708 | 15,792 | 106.9 |
| baseline_live10k | map | cancel | 686,762 | 83 | 125 | 208 | 500 | 37,000 | 92.0 |
| baseline_live10k | map | reduce | 75,471 | 42 | 84 | 125 | 334 | 12,458 | 60.0 |
| baseline_live10k | map | replace | 76,540 | 167 | 292 | 417 | 875 | 29,542 | 195.6 |
| baseline_live10k | map | reject | 261,551 | 41 | 42 | 84 | 250 | 12,875 | 27.7 |
| depth_live1k | pool | add_passive | 822,474 | 41 | 42 | 83 | 916 | 32,500 | 29.0 |
| depth_live1k | pool | add_aggressive | 77,328 | 41 | 42 | 83 | 875 | 12,541 | 27.4 |
| depth_live1k | pool | cancel | 691,879 | 41 | 42 | 84 | 208 | 19,292 | 31.2 |
| depth_live1k | pool | reduce | 75,684 | 41 | 42 | 84 | 1000 | 3,375 | 32.2 |
| depth_live1k | pool | replace | 77,112 | 42 | 84 | 125 | 1000 | 11,375 | 53.2 |
| depth_live1k | pool | reject | 255,523 | 0 | 42 | 42 | 84 | 16,583 | 21.7 |
| depth_live1k | map | add_passive | 822,474 | 42 | 84 | 125 | 209 | 44,791 | 58.7 |
| depth_live1k | map | add_aggressive | 77,328 | 42 | 167 | 250 | 459 | 15,250 | 76.3 |
| depth_live1k | map | cancel | 691,879 | 42 | 84 | 125 | 375 | 64,541 | 53.6 |
| depth_live1k | map | reduce | 75,684 | 42 | 42 | 84 | 291 | 11,375 | 33.9 |
| depth_live1k | map | replace | 77,112 | 125 | 208 | 292 | 666 | 14,042 | 124.6 |
| depth_live1k | map | reject | 255,523 | 0 | 42 | 42 | 125 | 8,917 | 20.6 |
| depth_live100k | pool | add_passive | 821,806 | 42 | 125 | 167 | 250 | 47,208 | 53.8 |
| depth_live100k | pool | add_aggressive | 77,860 | 42 | 125 | 167 | 250 | 11,208 | 54.4 |
| depth_live100k | pool | cancel | 687,729 | 83 | 167 | 209 | 333 | 41,166 | 77.3 |
| depth_live100k | pool | reduce | 75,479 | 83 | 167 | 209 | 333 | 30,750 | 77.2 |
| depth_live100k | pool | replace | 76,558 | 84 | 209 | 291 | 458 | 14,541 | 111.1 |
| depth_live100k | pool | reject | 260,568 | 42 | 84 | 125 | 209 | 11,500 | 40.0 |
| depth_live100k | map | add_passive | 821,806 | 84 | 167 | 209 | 1041 | 113,375 | 107.4 |
| depth_live100k | map | add_aggressive | 77,860 | 84 | 292 | 417 | 1291 | 18,959 | 121.7 |
| depth_live100k | map | cancel | 687,729 | 125 | 250 | 375 | 708 | 102,583 | 144.0 |
| depth_live100k | map | reduce | 75,479 | 83 | 167 | 250 | 500 | 18,792 | 95.9 |
| depth_live100k | map | replace | 76,558 | 250 | 417 | 583 | 1750 | 40,375 | 252.6 |
| depth_live100k | map | reject | 260,568 | 42 | 125 | 167 | 333 | 17,958 | 46.9 |
| depth_live1m | pool | add_passive | 822,079 | 83 | 125 | 208 | 292 | 532,417 | 76.8 |
| depth_live1m | pool | add_aggressive | 77,526 | 83 | 125 | 209 | 417 | 94,083 | 78.2 |
| depth_live1m | pool | cancel | 693,299 | 208 | 292 | 375 | 667 | 1,097,458 | 194.6 |
| depth_live1m | pool | reduce | 76,506 | 208 | 292 | 375 | 583 | 1,449,500 | 214.4 |
| depth_live1m | pool | replace | 77,203 | 209 | 334 | 458 | 917 | 391,917 | 229.7 |
| depth_live1m | pool | reject | 253,387 | 83 | 125 | 208 | 292 | 102,541 | 74.5 |
| depth_live1m | map | add_passive | 822,079 | 83 | 291 | 375 | 708 | 1,306,834 | 112.6 |
| depth_live1m | map | add_aggressive | 77,526 | 84 | 375 | 708 | 1542 | 35,875 | 133.3 |
| depth_live1m | map | cancel | 693,299 | 417 | 625 | 792 | 3500 | 1,612,583 | 428.9 |
| depth_live1m | map | reduce | 76,506 | 292 | 500 | 584 | 1792 | 556,125 | 317.4 |
| depth_live1m | map | replace | 77,203 | 459 | 792 | 1250 | 4875 | 523,250 | 523.3 |
| depth_live1m | map | reject | 253,387 | 125 | 417 | 500 | 792 | 111,542 | 174.5 |
| levels_4096_live100k | pool | add_passive | 825,559 | 42 | 125 | 208 | 458 | 1,059,875 | 70.8 |
| levels_4096_live100k | pool | add_aggressive | 74,107 | 42 | 125 | 167 | 250 | 22,166 | 52.4 |
| levels_4096_live100k | pool | cancel | 694,675 | 125 | 250 | 333 | 666 | 595,083 | 133.8 |
| levels_4096_live100k | pool | reduce | 76,258 | 125 | 250 | 333 | 625 | 19,625 | 131.1 |
| levels_4096_live100k | pool | replace | 77,375 | 167 | 292 | 416 | 958 | 26,500 | 171.9 |
| levels_4096_live100k | pool | reject | 252,026 | 42 | 125 | 167 | 250 | 28,125 | 45.3 |
| levels_4096_live100k | map | add_passive | 825,559 | 84 | 167 | 291 | 625 | 19,478,250 | 133.2 |
| levels_4096_live100k | map | add_aggressive | 74,107 | 83 | 209 | 334 | 1000 | 1,798,958 | 117.7 |
| levels_4096_live100k | map | cancel | 694,675 | 125 | 209 | 583 | 1042 | 12,246,083 | 160.8 |
| levels_4096_live100k | map | reduce | 76,258 | 84 | 167 | 458 | 959 | 6,144,292 | 196.0 |
| levels_4096_live100k | map | replace | 77,375 | 209 | 375 | 792 | 1709 | 170,333 | 242.0 |
| levels_4096_live100k | map | reject | 252,026 | 41 | 84 | 208 | 458 | 228,500 | 36.2 |
| symbols16_live100k | pool | add_passive | 821,840 | 42 | 84 | 166 | 250 | 43,750 | 45.1 |
| symbols16_live100k | pool | add_aggressive | 77,826 | 42 | 125 | 167 | 250 | 19,083 | 45.3 |
| symbols16_live100k | pool | cancel | 688,347 | 42 | 167 | 209 | 375 | 106,791 | 70.7 |
| symbols16_live100k | pool | reduce | 75,484 | 42 | 167 | 209 | 375 | 29,916 | 70.7 |
| symbols16_live100k | pool | replace | 76,666 | 84 | 208 | 291 | 541 | 16,000 | 100.9 |
| symbols16_live100k | pool | reject | 259,837 | 41 | 84 | 125 | 208 | 18,541 | 31.7 |
| symbols16_live100k | map | add_passive | 821,840 | 208 | 583 | 875 | 16792 | 131,992,333 | 603.6 |
| symbols16_live100k | map | add_aggressive | 77,826 | 291 | 1000 | 1792 | 22791 | 10,981,125 | 644.6 |
| symbols16_live100k | map | cancel | 688,347 | 375 | 833 | 1209 | 23833 | 36,256,083 | 704.6 |
| symbols16_live100k | map | reduce | 75,484 | 250 | 625 | 917 | 20709 | 1,465,542 | 392.6 |
| symbols16_live100k | map | replace | 76,666 | 542 | 1417 | 2791 | 27333 | 26,823,041 | 1120.1 |
| symbols16_live100k | map | reject | 259,837 | 84 | 458 | 625 | 11000 | 5,150,250 | 223.8 |
| symbols256_live100k | pool | add_passive | 822,692 | 42 | 125 | 167 | 375 | 22,583 | 58.0 |
| symbols256_live100k | pool | add_aggressive | 76,974 | 42 | 125 | 208 | 375 | 17,792 | 60.9 |
| symbols256_live100k | pool | cancel | 697,860 | 84 | 208 | 291 | 625 | 32,875 | 105.5 |
| symbols256_live100k | pool | reduce | 76,136 | 84 | 208 | 291 | 625 | 32,500 | 104.1 |
| symbols256_live100k | pool | replace | 77,767 | 125 | 250 | 375 | 917 | 10,833 | 146.9 |
| symbols256_live100k | pool | reject | 248,571 | 42 | 84 | 166 | 250 | 14,834 | 39.3 |
| symbols256_live100k | map | add_passive | 822,692 | 125 | 250 | 500 | 1208 | 1,467,042 | 167.1 |
| symbols256_live100k | map | add_aggressive | 76,974 | 166 | 459 | 833 | 2292 | 784,208 | 217.4 |
| symbols256_live100k | map | cancel | 697,860 | 167 | 375 | 750 | 1583 | 3,269,667 | 211.8 |
| symbols256_live100k | map | reduce | 76,136 | 125 | 292 | 666 | 1375 | 1,964,083 | 195.1 |
| symbols256_live100k | map | replace | 77,767 | 292 | 666 | 1209 | 5333 | 233,625 | 365.3 |
| symbols256_live100k | map | reject | 248,571 | 42 | 166 | 333 | 584 | 41,125 | 57.5 |
| randomids_live10k | pool | add_passive | 821,896 | 41 | 42 | 42 | 84 | 10,500 | 23.4 |
| randomids_live10k | pool | add_aggressive | 77,780 | 41 | 42 | 83 | 125 | 8,000 | 22.6 |
| randomids_live10k | pool | cancel | 686,762 | 41 | 42 | 84 | 167 | 16,000 | 29.8 |
| randomids_live10k | pool | reduce | 75,471 | 41 | 42 | 84 | 167 | 9,083 | 32.0 |
| randomids_live10k | pool | replace | 76,540 | 42 | 84 | 125 | 209 | 9,291 | 50.6 |
| randomids_live10k | pool | reject | 261,551 | 0 | 42 | 42 | 83 | 17,375 | 17.3 |
| randomids_live10k | map | add_passive | 821,896 | 83 | 125 | 208 | 1167 | 231,542 | 92.5 |
| randomids_live10k | map | add_aggressive | 77,780 | 83 | 250 | 375 | 1333 | 32,083 | 103.5 |
| randomids_live10k | map | cancel | 686,762 | 83 | 125 | 292 | 1084 | 587,416 | 84.9 |
| randomids_live10k | map | reduce | 75,471 | 42 | 84 | 167 | 1083 | 12,833 | 53.9 |
| randomids_live10k | map | replace | 76,540 | 167 | 292 | 459 | 1458 | 39,416 | 181.1 |
| randomids_live10k | map | reject | 261,551 | 41 | 42 | 84 | 459 | 85,792 | 28.6 |
| randomids_live1m | pool | add_passive | 822,079 | 83 | 167 | 291 | 542 | 487,167 | 100.0 |
| randomids_live1m | pool | add_aggressive | 77,526 | 83 | 208 | 333 | 625 | 451,208 | 105.2 |
| randomids_live1m | pool | cancel | 693,299 | 209 | 375 | 542 | 7125 | 137,500 | 243.9 |
| randomids_live1m | pool | reduce | 76,506 | 209 | 375 | 500 | 7500 | 63,709 | 245.4 |
| randomids_live1m | pool | replace | 77,203 | 250 | 458 | 666 | 9333 | 71,750 | 294.3 |
| randomids_live1m | pool | reject | 253,387 | 83 | 167 | 250 | 541 | 124,041 | 95.1 |
| randomids_live1m | map | add_passive | 822,079 | 167 | 500 | 666 | 1500 | 106,917 | 227.1 |
| randomids_live1m | map | add_aggressive | 77,526 | 208 | 542 | 834 | 3083 | 82,667 | 246.9 |
| randomids_live1m | map | cancel | 693,299 | 375 | 625 | 833 | 3667 | 111,250 | 404.5 |
| randomids_live1m | map | reduce | 76,506 | 291 | 500 | 625 | 1250 | 28,500 | 281.1 |
| randomids_live1m | map | replace | 77,203 | 459 | 833 | 1333 | 6000 | 78,500 | 507.8 |
| randomids_live1m | map | reject | 253,387 | 166 | 458 | 583 | 1000 | 39,042 | 189.4 |
| symbols256_live100k_oversized | pool | add_passive | 822,692 | 83 | 500 | 3000 | 20916 | 2,404,875 | 237.8 |
| symbols256_live100k_oversized | pool | add_aggressive | 76,974 | 83 | 500 | 2542 | 18875 | 290,625 | 210.4 |
| symbols256_live100k_oversized | pool | cancel | 697,860 | 166 | 375 | 1000 | 3375 | 4,653,292 | 211.8 |
| symbols256_live100k_oversized | pool | reduce | 76,136 | 166 | 375 | 1042 | 3875 | 670,334 | 204.6 |
| symbols256_live100k_oversized | pool | replace | 77,767 | 208 | 958 | 3042 | 23250 | 1,459,083 | 408.4 |
| symbols256_live100k_oversized | pool | reject | 248,571 | 42 | 167 | 917 | 3292 | 1,466,250 | 103.2 |
| symbols256_live100k_oversized | map | add_passive | 822,692 | 125 | 209 | 333 | 750 | 69,709 | 133.1 |
| symbols256_live100k_oversized | map | add_aggressive | 76,974 | 125 | 375 | 542 | 1250 | 24,458 | 163.3 |
| symbols256_live100k_oversized | map | cancel | 697,860 | 125 | 250 | 417 | 1000 | 42,417 | 160.2 |
| symbols256_live100k_oversized | map | reduce | 76,136 | 125 | 209 | 375 | 792 | 17,375 | 132.0 |
| symbols256_live100k_oversized | map | replace | 77,767 | 250 | 500 | 750 | 1917 | 32,833 | 295.6 |
| symbols256_live100k_oversized | map | reject | 248,571 | 42 | 125 | 208 | 375 | 31,125 | 45.4 |

### Measured event mix (first repetition of each config)

| Config | Commands | Accepted | Trades | Canceled | Reduced | Replaced | Rejected |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline_live10k | 2,000,000 | 899,676 | 210,646 | 686,762 | 75,471 | 76,540 | 261,551 |
| depth_live1k | 2,000,000 | 899,802 | 205,749 | 691,879 | 75,684 | 77,112 | 255,523 |
| depth_live100k | 2,000,000 | 899,666 | 210,217 | 687,729 | 75,479 | 76,558 | 260,568 |
| depth_live1m | 2,000,000 | 899,605 | 211,486 | 693,299 | 76,506 | 77,203 | 253,387 |
| levels_4096_live100k | 2,000,000 | 899,666 | 203,133 | 694,675 | 76,258 | 77,375 | 252,026 |
| symbols16_live100k | 2,000,000 | 899,666 | 209,730 | 688,347 | 75,484 | 76,666 | 259,837 |
| symbols256_live100k | 2,000,000 | 899,666 | 200,192 | 697,860 | 76,136 | 77,767 | 248,571 |
| randomids_live10k | 2,000,000 | 899,676 | 210,646 | 686,762 | 75,471 | 76,540 | 261,551 |
| randomids_live1m | 2,000,000 | 899,605 | 211,486 | 693,299 | 76,506 | 77,203 | 253,387 |
| symbols256_live100k_oversized | 2,000,000 | 899,666 | 200,192 | 697,860 | 76,136 | 77,767 | 248,571 |

## Single-operation microbenchmarks (me_bench, Google Benchmark)

Mean nanoseconds per item over a timed batch (median of the repetitions when repeated). An item is one command, except for Sweep where it is one fill. The argument is the number of resting orders in the book (Sweep: orders/fills per taker). cv is the coefficient of variation of the rate across repetitions: a difference between the two books that is not well clear of it is not a result.

| Operation | Book depth | map ns | map cv | pool ns | pool cv | map/pool |
|---|---:|---:|---:|---:|---:|---:|
| AddPassive | 1000 | 34.3 | 8% | 14.8 | 4% | 2.32x |
| AddPassive | 10000 | 35.4 | 15% | 27.7 | 6% | 1.28x |
| AddPassive | 100000 | 35.8 | 7% | 61.2 | 33% | 0.59x |
| AddPassive | 1000000 | 77.3 | 30% | 74.1 | 29% | 1.04x |
| Cancel | 1000 | 62.4 | 28% | 20.0 | 27% | 3.12x |
| Cancel | 10000 | 63.2 | 1% | 28.5 | 16% | 2.21x |
| Cancel | 100000 | 103.3 | 4% | 42.5 | 5% | 2.43x |
| Cancel | 1000000 | 159.1 | 13% | 51.3 | 5% | 3.10x |
| AggressiveMatch | 1000 | 25.9 | 2% | 8.9 | 29% | 2.91x |
| AggressiveMatch | 10000 | 26.9 | 7% | 13.5 | 14% | 1.99x |
| AggressiveMatch | 100000 | 26.9 | 1% | 11.4 | 4% | 2.36x |
| AggressiveMatch | 1000000 | 28.2 | 4% | 14.2 | 9% | 1.99x |
| Sweep | 10000/8 | 49.1 | 1% | 8.3 | 9% | 5.88x |
| Sweep | 10000/64 | 49.9 | 4% | 7.3 | 2% | 6.85x |
| Sweep | 10000/512 | 37.8 | 17% | 5.7 | 5% | 6.68x |

