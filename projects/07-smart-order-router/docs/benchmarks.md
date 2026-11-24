# Benchmarks & Latency Analysis

All measurements below were collected directly on this machine using `./scripts/run_benchmarks.sh`. Raw outputs and full hardware/compiler metadata live in `results/benchmarks/`.

## Environment Metadata

Source: [environment.json](../results/benchmarks/environment.json)

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (10 physical / 10 logical cores, arm64) |
| OS / kernel | macOS Darwin 25.5.0 |
| Compiler / version / flags | Apple clang 21.0.0 (`-O3 -DNDEBUG -std=c++20`) |
| Dataset / event mix / sample count | Synthetic L2 books (5 levels/side/venue), alternating Buy/Sell IOC requests (`100..1500` shares), 200,000 timed samples (`seed=42`) |
| Warmup / affinity / governor | 20,000 warmup ops; default macOS scheduling (no user-space CPU governor or core pinning) |
| Timing methodology | `std::chrono::steady_clock` over batches of 16 ops per timing sample to stay well above Apple Silicon timer tick granularity (~41.7 ns), with `volatile` sink preventing dead-code elimination |

## Measured Hot-Path Results

Source: [router_latency.json](../results/benchmarks/router_latency.json)

| Benchmark | Venues | Depth | Throughput (ops/sec) | Mean (ns) | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `route_plan_only` | 3 | 5 | 1,381,557 | 722.5 | 640.6 | 989.6 | 1,533.9 | 8,984.4 | 111,773.4 |
| `route_plan_only` | 5 | 5 | 1,008,119 | 990.7 | 849.0 | 1,380.2 | 2,687.6 | 12,754.2 | 76,838.6 |
| `route_plan_only` | 8 | 5 | 1,007,478 | 991.6 | 973.9 | 1,028.7 | 1,328.2 | 4,087.4 | 18,315.1 |
| `wire_decode_update_route` | 3 | 5 | 1,288,353 | 775.3 | 763.1 | 783.8 | 955.7 | 2,315.1 | 5,070.3 |

## Observations

1. **Sub-microsecond median routing**: zero-allocation stack buffers (`std::array`) in `ConsolidatedBook` and `RoutePlanner` keep 3-venue 5-level `SmartCostAware` planning at **640.6 ns median** (~1.38M decisions/sec) and full wire-decode + FNV-1a checksum + normalize + L2 book upsert + route plan at **763.1 ns median / 955.7 ns p99** (~1.29M end-to-end ticks/sec).
2. **Predictable scaling with venue count**: scaling from 3 venues (15 candidate levels) to 8 venues (40 candidate levels) increases median decision latency smoothly from **640.6 ns** to **973.9 ns** (~1.52x for 2.67x more levels).
3. **OS jitter in upper tails**: without isolated Linux cores (`isolcpus` / `taskset`), background macOS scheduler preemption accounts for occasional multi-microsecond `p99.9`/`max` spikes.

## Reproduction Command

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./scripts/run_benchmarks.sh
```
