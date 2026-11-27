# Performance Engineering Report

All measurements in this report were collected directly from the compiled `hft_bench` binary (`engine/apps/hft_bench.cpp`, backed by Project 09 `lpl::bench_harness` and `lpl::environment`). No standalone module README numbers are substituted for integrated measurements.

---

## 1. Hardware, Compiler & Methodology Provenance

| Attribute | Value |
|---|---|
| Raw artifact directory | [`integration/results/apple_m5_release/`](../integration/results/apple_m5_release/) |
| Baseline Git SHA | `8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89` |
| Host CPU | Apple M5 (`arm64`, 10 logical cores) |
| Operating System | macOS 26.5.1 (Darwin 25.5.0) |
| Compiler & Flags | AppleClang 21.0.0 (`Release`, `-O3 -DNDEBUG`, `-std=c++20`) |
| Clock Domain | `WallTimeNs` (`std::chrono::steady_clock`, calibrated read overhead `19.17 ns`) |
| Warmup & Sampling | 2 warmup rounds + 5 repetitions × 20 samples (`events_per_sample = 500`, `10,000` for steady-state matching); 100 warmup + 500 measured ops for per-event E2E latency histograms |
| Platform Notes | Apple Silicon `steady_clock` ticks at 24 MHz (~41.67 ns quantization), visible in single-op E2E percentiles. Linux `perf_event_open` hardware cache/branch counters and `pthread_setaffinity_np` pinning are unavailable on macOS; `lpl::PerfCounterGroup` uses `proc_pid_rusage(RUSAGE_INFO_V4)` for cycles/instructions and reports `-1` for unsupported counters. Linux perf experiments are labeled separately in CI. |

**Reproduce locally:**
```bash
./scripts/run_benchmarks.sh integration/results/apple_m5_release
```

---

## 2. Per-Stage Latency & Throughput Breakdown

Measured with `WallTimeNs` in `performance` mode (`integration/results/apple_m5_release/summary.csv`):

| Stage | Benchmark | Variant | p50 (ns/op) | p99 (ns/op) | Throughput (Mops/s) | IPC | RepCV% |
|---|---|---|---:|---:|---:|---:|---:|
| 1. Feed Decode | `01_itch_decode` | `project02_adapter` | 135.75 | 191.21 | 7.37 | 2.73 | 4.32% |
| 2. Book Update | `02_book_update` | `canonical_l3_book` | 39.92 | 54.56 | 25.05 | 4.01 | 11.97% |
| 3. Strategy Compute | `03_strategy_compute` | `mmsim_inventory_mm` | 14.50 | 18.04 | 68.97 | 3.74 | 0.54% |
| 4. Pre-Trade Risk | `04_pretrade_risk` | `risk_gate_check` | 107.17 | 130.21 | 9.33 | 2.92 | 4.71% |
| 5. Smart Routing | `05_smart_routing` | `sor_cost_aware_3v` | 639.04 | 963.37 | 1.56 | 2.97 | 20.15% |
| 6. Wire Codec | `06_wire_serialization` | `ouch_roundtrip` | 22.42 | 22.71 | 44.61 | 7.38 | 0.17% |
| 7a. Matching (Pool) | `07_matching_steady` | `pool_book_10k` | 16.24 | 27.99 | 61.58 | 5.40 | 11.37% |
| 7b. Matching (Map) | `07_matching_steady` | `map_book_10k` | 34.12 | 49.68 | 29.31 | 4.84 | 11.84% |
| 8. OMS + Ledger | `08_oms_and_ledger` | `fixed_point_ledger` | 96.79 | 111.56 | 10.33 | 5.52 | 3.10% |

---

## 3. End-to-End Distribution Breakdown (Separate Timing Metrics)

Measured independently across 500 post-warmup operations on the hot path (`integration/results/apple_m5_release/e2e_latency.json`):

| End-to-End Metric | Path Covered | Samples | p50 (ns) | p95 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---:|---:|---:|---:|---:|---:|
| `tick_to_decision` | Canonical book update + strategy order intent + `PreTradeRiskGate` | 500 | 333.0 | 584.0 | 1,125.0 | 2,938.8 | 3,583.0 |
| `order_submit_to_ack` | `OmsReconciler::on_order_sent` + Project 01 `PoolBook` match + Ack handling | 500 | 125.0 | 208.0 | 791.4 | 3,023.4 | 4,458.0 |
| `order_submit_to_fill` | `OmsReconciler` + Project 01 `PoolBook` match + fill + fixed-point `AccountingLedger` | 500 | 166.5 | 250.0 | 875.8 | 4,752.3 | 6,166.0 |

---

## 4. Comparative Investigations

### Investigation A — `PoolBook` vs `MapBook` Matching (Steady-State vs Cold-Start Allocation)

| Workload | `PoolBook` p50 / p99 | `MapBook` p50 / p99 | Checksum Match | Observation |
|---|---|---|---|---|
| Steady-state 10,000 L3 events (`max_orders=4096`) | **16.24 ns / 27.99 ns** (61.58 Mops/s) | 34.12 ns / 49.68 ns (29.31 Mops/s) | **YES** | `PoolBook` achieves **2.10× faster p50** and **1.77× tighter p99** by eliminating per-order heap allocations and tree pointer chasing. |
| Short 107-event full-pipeline cold start (`max_orders=65,536` constructed per run) | 2,750.78 ns / 3,181.81 ns | **234.81 ns / 295.38 ns** | **YES** (`66d95957646657a6`) | Upfront flat-pool zero-initialization (~270 µs) dominates short ephemeral fixtures when engine construction is inside the timed region. |

**Engineering conclusion:** Preallocated pool structures are strictly superior for long-running trading sessions, *provided* pool capacity is sized appropriately and allocated once at process startup rather than recreated per micro-batch.

### Investigation B — Direct In-Process vs Two-Thread Bounded SPSC Transport (`lfm::SpscQueue`)

| Transport Mode | p50 (ns/op) | p99 (ns/op) | Throughput (Mops/s) | Drops | Book Hash Match |
|---|---:|---:|---:|---:|---|
| `direct_in_process` | 15.75 | 17.69 | 63.49 | 0 | Baseline |
| `spsc_two_thread` (Project 05 `lfm::SpscQueue<Envelope>`) | 54.46 | 84.02 | 18.36 | 0 | **YES** |

**Engineering conclusion:** Cross-thread cache-line handoff adds ~39 ns median latency and widens p99 due to inter-core coherence and OS thread preemption (especially without strict Linux core isolation). Use direct in-process transport for deterministic backtesting/CI, and reserve SPSC handoff for true I/O-to-compute thread boundaries.

### Investigation C — Clean vs 15%-Loss Recoverable MDF1 Multicast (`projects/06`)

| Multicast Condition | p50 (ns/op) | p99 (ns/op) | Throughput (Mops/s) | Final State Hash Match |
|---|---:|---:|---:|---|
| `clean_no_loss` | 253.12 | 278.13 | 3.95 | **YES** |
| `loss_15pct_recovered` (15% packet drops + 10% reordering + gap detection + retransmit) | 310.75 | 322.46 | 3.22 | **YES** |

**Engineering conclusion:** Adding CRC32 validation, sequence gap tracking, reordering buffers, and in-memory retransmit recovery adds **+22.8% median overhead** (+57.6 ns/op) while guaranteeing exact publisher/subscriber book convergence and safely gating strategy execution during out-of-sync windows.
