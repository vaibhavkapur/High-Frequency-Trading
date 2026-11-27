# Operator & Developer Runbook

## 1. Prerequisites & Build

- **Required:** C++20 compiler (Apple Clang 15+, Clang 14+, or GCC 11+), CMake >= 3.20, Make or Ninja.
- **Optional (Linux multi-NUMA):** `libnuma-dev`, `numactl`, `linux-tools-common` (`perf`), Google Benchmark (`libbenchmark-dev`).

```bash
cd projects/14-numa-aware-trading-engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 2. One-Command Demo & Full Validation

```bash
./scripts/run_demo.sh
```

This runs:
1. Release C++20 CMake configure & build.
2. Full CTest suite (4 unit & integration test executables).
3. Canonical fixture schema validation.
4. Machine topology probe (`reports/topology.json`) + repeated placement benchmark (`reports/raw_benchmark_results.json` & `reports/benchmark_summary.csv`).
5. Automated acceptance verification (`scripts/verify_acceptance.py`).

## 3. Common CLI Workflows

### Inspect Machine Topology
```bash
./build/numa_trading_engine --mode topology
```

### Run Comparison Suite (Baseline vs. Placements)
```bash
./build/numa_trading_engine --mode compare \
  --events 200000 --warmup 10000 --capacity 4096 --repetitions 5 --seed 42 \
  --output reports/raw_benchmark_results.json
```

### Replay Deterministic Fixture with JSON Config
```bash
./build/numa_trading_engine --mode run \
  --config fixtures/config_valid.json \
  --fixture fixtures/valid_small_events.csv
```

### Exercise Backpressure Scenario
```bash
./build/numa_trading_engine --mode run \
  --pipeline lock_free_spsc --policy first_touch_local \
  --events 20000 --warmup 500 --capacity 16 --consumer-work 60 --allow-fallback
```

### Validate Invalid Fixtures (Expected Non-Zero Exit)
```bash
./build/numa_trading_engine --mode validate-fixture --fixture fixtures/invalid_negative_price.csv
# Expected stderr: VALIDATION_FAILED [INVALID_PRICE] field=price_ticks line=4: ...
```

## 4. Multi-NUMA Linux Benchmarking Guide

On a multi-socket Linux host (2+ NUMA nodes):

1. Verify topology:
   ```bash
   numactl --hardware
   ./build/numa_trading_engine --mode topology
   ```
   Confirm `"is_multi_numa": true` and `"has_libnuma": true`.
2. Enable unprivileged hardware PMU counters (if permitted on test server):
   ```bash
   cat /proc/sys/kernel/perf_event_paranoid
   # Requires <= 1 (or CAP_PERFMON / sudo) for perf_event_open LLC cache miss counters
   ```
3. Run the comparison suite (all 6 scenarios including `spsc_remote_node` and `spsc_interleaved` will execute natively with `status: "EXECUTED"`):
   ```bash
   ./build/bench_placements --events 500000 --warmup 20000 --capacity 4096 --reps 7 --seed 42
   ```
4. Optionally wrap with Linux `perf stat` for independent cross-check of LLC misses and NUMA traffic:
   ```bash
   perf stat -e cache-references,cache-misses,LLC-load-misses,migrations,context-switches \
     ./build/numa_trading_engine --mode run --pipeline lock_free_spsc --policy remote \
     --producer-cpu 0 --consumer-cpu 16 --memory-node 1 --events 500000 --warmup 20000
   ```

## 5. Diagnostics & Failure Recovery

| Symptom / Error Code | Cause | Recovery Action |
|---|---|---|
| `UNAVAILABLE_SINGLE_NODE` | `remote` or `interleaved` policy requested on a 1-node host in strict mode | Expected on single-node machines. Run on a >=2-node Linux server for cross-node metrics, or pass `--allow-fallback` only for functional testing (output will be labeled `FALLBACK_SINGLE_NODE`). |
| `UNAVAILABLE_PLATFORM_API` | `--strict-pinning` passed on macOS | Remove `--strict-pinning` on macOS (Mach affinity tags are advisory only) or run on Linux where `pthread_setaffinity_np` enforces hard pinning. |
| `INVALID_QUEUE_CAPACITY` | Non-power-of-two capacity passed to SPSC pipeline | Pass a power of two in `[8, 4194304]` (e.g., `16`, `1024`, `4096`). |
| `INVALID_CONFIG` / `FILE_NOT_FOUND` | Missing config file or omitted required JSON key | Verify `--config` path and ensure `scenario_name`, `pipeline_kind`, `memory_policy`, and `queue_capacity` are present. |
| `hw_counters_available: false` | Running on macOS or Linux with `perf_event_paranoid > 1` / restricted VM PMU | Normal on macOS and restricted cloud VMs; OS `getrusage` counters still populate. On dedicated Linux bare metal, set `perf_event_paranoid=1` or run with `CAP_PERFMON`. |
| High variance in latency | CPU frequency scaling, turbo boost, or noisy neighbor processes | Pin CPU governor to `performance` on Linux (`cpupower frequency-set -g performance`), isolate cores (`isolcpus`), and increase `--repetitions`. |
