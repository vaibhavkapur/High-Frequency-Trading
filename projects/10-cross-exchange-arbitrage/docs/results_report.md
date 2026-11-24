# M5 Experimental Results & Performance Report

All numbers below were measured directly on local hardware using reproducible commands (`scripts/run_benchmarks.sh`). No values are fabricated or extrapolated to live markets.

## 1. Environment & Methodology

- **Hardware:** Apple M5 (10 cores, ARM64), 16 GiB unified memory
- **OS / Kernel:** macOS Darwin 25.5.0 (`RELEASE_ARM64_T8142`)
- **Compiler / Flags:** Apple Clang 21.0.0 (`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`)
- **Dataset (Baseline & Sweeps):** `results/sample_feed.xarb` — 29,577 events across 3 venues (`binance=10,004`, `coinbase=9,582`, `okx=9,991`), 50.00 s simulated span, `seed=20261124`, with injected faults (1 Binance sequence gap + 45 ms REST resync, 1 Coinbase 2.1 s silent outage, 1 OKX CRC32 checksum corruption + 60 ms WS reconnect)
- **Reproduction Command:** `./scripts/run_benchmarks.sh`

---

## 2. Invalidated vs. Actionable Signals (Baseline: 5 ms Order Latency, Limit-IOC)

A key finding is how aggressively naive top-of-book crosses collapse once staleness, synchronization state, taker fees (18–22 bps round-trip), pre-funded balances, and cooldowns are enforced (`results/baseline_5ms/summary.json`, run hash `ba162e6c2f9815db`):

| Signal Disposition | Count | Share of Gross Crosses |
|---|---:|---:|
| **Gross top-of-book crosses observed** | **24,314** | **100.00%** |
| Rejected — `insufficient_edge_after_fees` | 13,300 | 54.70% |
| Rejected — `insufficient_funding` (pre-funded balance depleted) | 6,672 | 27.44% |
| Rejected — `below_min_profit` (< 0.25 USDT net) | 2,489 | 10.24% |
| Rejected — `cooldown_active` | 1,255 | 5.16% |
| Rejected — `venue_not_synced` (gap / CRC32 resync) | 482 | 1.98% |
| Rejected — `stale_quote` (silent outage) | 4 | 0.02% |
| **Actionable signals emitted** | **112** | **0.46%** |

Of the **112** actionable signals submitted to the paper simulator at 5 ms order latency:
- **40** (`35.71%` fill rate) completed both legs profitably (mean gross spread `21.08 bps`, mean net executable edge on actionable signals `21.46 bps`),
- **54** became **one-legged unhedged** fills because one venue's quote moved before arrival,
- **1** missed both legs completely,
- **17** were rejected as unfunded due to in-flight reservations from overlapping signals,
- **False-positive rate among completed trades:** `57.89%` (`55 / 95`),
- **Net simulated paper P&L:** `+2,994.14 USDT` (`+2,451.47` matched + `+542.67` unhedged MTM; `3,021.53 USDT` taker fees paid; `41.83 USDT` max drawdown; accounting invariants `PASS`; SQLite & CSV cross-check verified).

---

## 3. Venue Recovery & Staleness Metrics

Measured during the baseline replay (`results/baseline_5ms/summary.json`):

| Venue | Messages | Deltas Applied | Snapshots | Gaps | CRC32 Errors | Resyncs | Total Stale Time | Reconnect p50 / p99 | Staleness p50 / p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **Binance** | 10,001 | 10,000 | 2 | 1 | 0 | 1 | 45.00 ms | 22.75 / 44.56 ms | 2.67 / 983.39 ms |
| **Coinbase** | 9,581 | 9,579 | 2 | 0 | 0 | 0 | 610.72 ms | 5.00 / 5.00 ms | 2.69 / 1,501.58 ms |
| **OKX** | 9,990 | 9,987 | 2 | 0 | 1 | 1 | 63.17 ms | 63.17 / 63.17 ms | 2.51 / 983.03 ms |

All three venue-specific recovery rules detected their respective injected faults, suppressed signals during recovery (`486` gross crosses suppressed across unsynced/stale windows), and returned cleanly to `Synced`.

---

## 4. Latency Stress Sweep: False-Positive Rate & Paper P&L

Sweeping deterministic order submission latency from `0 ms` to `100 ms` on the exact same 29,577-event feed exposes the core execution risk of cross-exchange arbitrage:

### 4.1 Limit-IOC Mode (Protects Price, Exposes One-Legged / Miss Risk)
Raw file: [`results/latency_sweep_limit_ioc.csv`](../results/latency_sweep_limit_ioc.csv)

| Order Latency | Actionable Signals | Profitable | One-Legged | Both Missed | Fill Rate | False-Positive Rate | Net Paper P&L (USDT) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 ms | 494 | 448 | 0 | 0 | 90.69% | 0.00% | +21,884.99 |
| 1.0 ms | 402 | 240 | 96 | 0 | 59.70% | 28.57% | +13,886.41 |
| 2.0 ms | 402 | 240 | 96 | 0 | 59.70% | 28.57% | +13,886.41 |
| 5.0 ms | 112 | 40 | 54 | 1 | 35.71% | 57.89% | +2,994.14 |
| 10.0 ms | 88 | 11 | 56 | 5 | 12.50% | 84.72% | +1,249.21 |
| 25.0 ms | 86 | 1 | 49 | 20 | 1.16% | 98.57% | +194.62 |
| 50.0 ms | 58 | 0 | 31 | 16 | 0.00% | 100.00% | +164.45 |
| 100.0 ms | 95 | 0 | 37 | 32 | 0.00% | 100.00% | -276.66 |

### 4.2 Market-IOC Mode (Guarantees Fills, Exposes Adverse Slippage Risk)
Raw file: [`results/latency_sweep_market_ioc.csv`](../results/latency_sweep_market_ioc.csv)

| Order Latency | Actionable Signals | Profitable | Slippage-Loss | One-Legged | Fill Rate | False-Positive Rate | Net Paper P&L (USDT) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 ms | 602 | 366 | 0 | 0 | 60.80% | 0.00% | +19,194.36 |
| 1.0 ms | 602 | 362 | 4 | 0 | 60.80% | 1.09% | +18,770.43 |
| 5.0 ms | 602 | 331 | 35 | 0 | 60.80% | 9.56% | +15,388.82 |
| 10.0 ms | 602 | 258 | 98 | 0 | 59.14% | 27.53% | +8,206.21 |
| 25.0 ms | 605 | 138 | 215 | 0 | 58.35% | 60.91% | **-4,130.97** |
| 50.0 ms | 610 | 117 | 233 | 0 | 57.38% | 66.57% | **-7,089.71** |
| 100.0 ms | 579 | 35 | 327 | 1 | 62.52% | 90.36% | **-16,290.52** |

**Takeaway:** Under Market-IOC, paper P&L flips sharply negative by **25 ms** (`-4,130.97 USDT`) because fleeting dislocations revert before orders arrive, turning apparent arbitrage into double taker-fee + spread crossing losses. Under Limit-IOC, price limits curb outright slippage losses, but by **10 ms** `84.72%` of completed attempts degrade into unhedged one-legged inventory risk or misses.

---

## 5. Measured Performance Tradeoff: `FlatOrderBook` vs. `MapOrderBook`

Workload: 400,000 mixed operations (80% L2 price-level updates/deletes, 20% depth-aware VWAP walks), 20,000-op warmup, batched in groups of 16 ops (`results/book_tradeoff.csv`):

| Implementation | Book Depth | Throughput (M ops/s) | p50 (ns/op) | p99 (ns/op) | p99.9 (ns/op) | Relative Speedup |
|---|---:|---:|---:|---:|---:|---:|
| `FlatOrderBook` | 20 | **44.30** | **18.2** | 65.1 | 101.6 | +8.3% |
| `MapOrderBook` | 20 | 40.92 | 23.4 | 33.8 | 101.6 | baseline |
| `FlatOrderBook` | 50 | **51.36** | **18.2** | **26.1** | **88.6** | **+36.6%** |
| `MapOrderBook` | 50 | 37.61 | 26.0 | 33.9 | 179.7 | baseline |
| `FlatOrderBook` | 200 | **30.79** | **26.0** | 54.7 | 942.8 | +6.4% |
| `MapOrderBook` | 200 | 28.95 | 31.2 | 49.5 | 682.3 | baseline |

**Engineering insight:** At typical crypto arbitrage top-50 L2 book depth, contiguous `FlatOrderBook` outperforms `std::map` by **+36.6% throughput** and halves p99.9 tail latency (`88.6 ns` vs `179.7 ns`) by eliminating pointer chasing and per-node heap allocations. At depth 200, the advantage shrinks (`+6.4%`) and `FlatOrderBook`'s p99.9 (`942.8 ns`) crosses above `MapOrderBook` (`682.3 ns`) due to `O(N)` `memmove` on inserts/deletes near top-of-book — confirming why capping L2 book depth to the executable top-50 levels is the right systems tradeoff here.

---

## 6. End-to-End Pipeline & Codec Throughput

Measured on 44,577 full JSON wire events (`results/e2e_benchmark.json`):
- **End-to-end replay** (JSON parse + OKX IEEE CRC32 + L2 book update + 6-route depth-aware VWAP scan + paper simulation): **54,955 events/s** (`54,937 book updates/s`), processing latency **p50 8.04 us / p99 26.75 us / p99.9 81.80 us**.
- **Feed codec decode throughput:** CRC32-validated binary (`XARB`) decodes at **306,012 events/s** (`0.1457 s`) vs. JSONL at **160,336 events/s** (`0.2780 s`) — **1.91x faster** while verifying frame integrity on every event.
