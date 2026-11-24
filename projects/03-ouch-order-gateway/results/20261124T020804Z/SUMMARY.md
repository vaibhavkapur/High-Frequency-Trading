# Benchmark results: 20261124T020804Z

Localhost only: gateway and exchange simulator in one process, loopback TCP.
These figures describe this machine and this simulator, not a real venue.

## Environment

```text
date_utc: 2026-11-24T02:08:04Z
git_revision: not a git repository
git_dirty: 0 files
os: Darwin 25.5.0 arm64
os_version: macOS 26.5.1
cpu: Apple M5
cores_physical: 10
cores_logical: 10
cores_performance: 4
cores_efficiency: 6
memory_bytes: 17179869184
governor: n/a (macOS; no user-selectable CPU frequency governor)
affinity: none (macOS has no thread-to-core pinning API)
power: Now drawing from 'Battery Power'
compiler: Apple clang version 21.0.0 (clang-2100.1.1.101)
cmake: cmake version 4.5.1
build_type: Release
cxx_flags: -O3 -DNDEBUG -std=c++20
orders_per_run: 200000
warmup_orders: 20000
command: scripts/run_benchmarks.sh 200000
```

## Submit-to-ack round trip and throughput

| Run | Window | Spin | NODELAY | Journal | Requests/s | p50 us | p90 us | p99 us | p99.9 us | max us | Gateway CPU % | Queue depth mean / max |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline_closed_loop | 1 | 0 | 1 | none | 61154 | 15.3 | 18.5 | 25.7 | 59.9 | 4843.3 | 36.6 | 1.00 / 1 |
| spin_closed_loop | 1 | 1 | 1 | none | 59031 | 15.8 | 19.0 | 28.2 | 65.9 | 4093.8 | 44.0 | 1.00 / 1 |
| nagle_closed_loop | 1 | 0 | 0 | none | 60673 | 15.3 | 18.5 | 29.9 | 78.2 | 4344.3 | 36.3 | 1.00 / 1 |
| nagle_window8 | 8 | 0 | 0 | none | 254919 | 27.3 | 35.8 | 46.4 | 94.5 | 4265.0 | 60.3 | 5.68 / 8 |
| nodelay_window8 | 8 | 0 | 1 | none | 237791 | 21.8 | 31.3 | 71.0 | 437.8 | 52949.5 | 74.5 | 6.24 / 8 |
| journal_buffered | 1 | 0 | 1 | buffered | 60737 | 15.1 | 18.1 | 25.5 | 64.3 | 4340.8 | 38.5 | 1.00 / 1 |
| journal_flush | 1 | 0 | 1 | flush | 44051 | 17.1 | 20.5 | 30.0 | 93.4 | 4827.2 | 54.9 | 1.00 / 1 |
| throughput_window64 | 64 | 0 | 1 | none | 408649 | 81.9 | 140.5 | 191.8 | 1503.0 | 5925.3 | 96.9 | 38.17 / 64 |
| throughput_window64_spin | 64 | 1 | 1 | none | 409884 | 81.8 | 138.8 | 184.5 | 1538.9 | 3775.3 | 97.2 | 38.14 / 64 |
| rate_20k_per_s | 1024 | 0 | 1 | none | 22041 | 62.1 | 104.9 | 464.6 | 16313.2 | 39258.2 | 16.4 | 9.68 / 896 |

## Behaviour under injected faults (simulated)

| Run | Fault rate | Requests/s | p50 us | p99 us | p99.9 us | Disconnects | Duplicates | Reorders | Delays | Uncertain | Resends | Duplicates dropped | Mismatches vs exchange |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| faults_0pct | 0% | 379104 | 30.5 | 63.2 | 240.0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| faults_1pct | 1% | 19638 | 129.5 | 3100.9 | 104630.5 | 2363 | 3748 | 3810 | 3788 | 33500 | 37808 | 3748 | 0 |
| faults_5pct | 5% | 7774 | 1476.8 | 7666.7 | 17792.6 | 14173 | 17932 | 18013 | 18709 | 132602 | 226768 | 17932 | 0 |

## Codec only

| Operation | Bytes | min ns/op | median ns/op |
|---|---|---|---|
| encode EnterOrder | 49 | 2.14 | 2.21 |
| decode EnterOrder | 49 | 1.73 | 1.75 |
| encode ReplaceOrder | 47 | 1.62 | 1.64 |
| decode ReplaceOrder | 47 | 1.47 | 1.50 |
| encode CancelOrder | 19 | 0.81 | 0.82 |
| decode CancelOrder | 19 | 0.71 | 0.74 |
| encode Accepted | 66 | 2.53 | 2.54 |
| decode Accepted | 66 | 1.95 | 1.97 |
| encode Replaced | 80 | 2.75 | 2.79 |
| decode Replaced | 80 | 2.62 | 2.69 |
| encode Executed | 40 | 1.33 | 1.33 |
| decode Executed | 40 | 1.20 | 1.21 |
| encode Canceled | 28 | 1.01 | 1.03 |
| decode Canceled | 28 | 1.12 | 1.21 |
| encode Rejected | 24 | 0.97 | 0.98 |
| decode Rejected | 24 | 1.20 | 1.22 |
| deframe+decode Executed | 43 | 2.50 | 2.62 |

13 gateway runs, 2335876 RTT samples in total. Raw samples: `rtt_<run>.csv`.
Per-run records: `gateway_bench.jsonl`. Console output: `gateway_bench.log`, `codec_bench.log`.

Runs whose final order state disagreed with the exchange: none.
