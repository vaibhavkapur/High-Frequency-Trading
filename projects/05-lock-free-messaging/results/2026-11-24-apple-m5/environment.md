# Benchmark environment

- date (UTC): 2026-11-24T01:48:49Z
- source revision: not a git checkout (dirty or untracked)
- CPU: Apple M5
- cores: 10 logical (4 performance + 6 efficiency)
- cache line: 128 bytes
- memory: 16 GiB
- OS: macOS 26.5.1 (Darwin 25.5.0)
- power: 'Battery Power', low power mode: 0
- frequency governor: not applicable (macOS manages P/E-core scheduling and DVFS)
- load average at start: 7.39 11.77 10.46
- compiler: /usr/bin/c++ — Apple clang version 21.0.0 (clang-2100.1.1.101)
- flags: -std=c++20 -O3 -DNDEBUG
- placement flags: --producer-qos p --consumer-qos p
- clock: clock_gettime_nsec_np(CLOCK_UPTIME_RAW); step and read cost are measured per run and recorded in latency.csv

## Commands

```
build/benchmarks/bench_throughput --messages 4000000 --runs 5 --producer-qos p --consumer-qos p --csv results/2026-11-24-apple-m5/throughput.csv
build/benchmarks/bench_latency --seconds 3 --max-samples 5000000 --warmup 100000 --producer-qos p --consumer-qos p --csv results/2026-11-24-apple-m5/latency.csv
build/benchmarks/bench_micro --benchmark_repetitions=5 --benchmark_report_aggregates_only=false --benchmark_out=results/2026-11-24-apple-m5/micro.json --benchmark_out_format=json
```
- load average at end: 17.32 12.46 10.82
