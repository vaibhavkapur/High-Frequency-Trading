# Cross-Exchange Arbitrage Detection & Paper Execution Engine

> Normalize asynchronous order books and test whether quoted price differences survive costs and latency.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20 engine, CMake, SQLite3, JSONL/CRC32 binary feeds, Python analytics  
**Status:** Implemented (roadmap M1–M5). All published numbers are measured on deterministic **synthetic** multi-venue feeds on local hardware; none is a claim about live market profitability. See [Caveats / non-goals](#caveats--non-goals).

## Why build this?

Build a paper-only arbitrage detector across multiple crypto exchange **public market-data** schemas (Binance, Coinbase, OKX). Evaluate feasibility under fees, L2 depth, pre-funded inventory, staleness, and execution latency—not just displayed top-of-book spread.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/demo.sh
```

Sample end-to-end replay output ([docs/sample_output.txt](docs/sample_output.txt)):

```text
feed               examples/sample_feed.xarb (7077 events, span 12.57 s)
scenario           demo_5ms
throughput         44290.8 events/s (44203.2 book updates/s, wall 0.1598 s)
proc latency       p50 8500 ns, p99 47975 ns, p99.9 131800 ns
signals            gross crosses 6521, actionable 121 (stale 188, unsynced 302, fees/edge 3852, depth 0, unfunded 539, cooldown 1474)
spreads (mean)     gross 15.93 bps vs net executable 17.78 bps
paper trades       attempted 121, profitable 44, slippage-loss 0, one-legged 57, missed 6, unfunded 14
rates              fill rate 36.3636%, false-positive rate 58.8785%
paper pnl          net 3054.50 USDT (matched 2581.40, unhedged mtm 473.10, fees 3362.62, max dd 55.48)
invariants         PASS (equity delta 3360.25 USDT)
venue[binance]     msgs 2501, deltas 2500, snaps 2, gaps 1, crc_err 0, resyncs 1, stale_ms 45.00
venue[coinbase]    msgs 2081, deltas 2079, snaps 2, gaps 0, crc_err 0, resyncs 0, stale_ms 384.80
venue[okx]         msgs 2490, deltas 2487, snaps 2, gaps 0, crc_err 1, resyncs 1, stale_ms 60.85
run hash           566a983ae36db0b4
```

Re-running with the same feed and seed produces the exact same 16-hex-digit `run hash` across both JSONL and binary (`XARB`) inputs.

## Architecture

```text
Binance WS/REST --\
Coinbase WS ------+--> [SPSC queues] -> venue adapters -> normalized L2 books -> opportunity scanner
OKX WS -----------/                       (per-venue        (FlatOrderBook)             |
                                        gap/CRC32 FSM)                           risk/cost model
                                                                                        |
                                                                           paper order/fill simulator
                                                                            (delayed legs vs truth)
                                                                                        |
                                                                           audit + analytics (SQLite/CSV)
```

Detailed protocol rules, state machines, and VWAP math: [docs/design.md](docs/design.md). Explicit modeling simplifications and bias directions: [docs/assumptions.md](docs/assumptions.md).

## Supported feature set

| Area | Implemented |
|---|---|
| **Venue adapters** | Three isolated adapters: **Binance** (WS delta buffering + REST `lastUpdateId` bridge + `U == prev_u + 1`), **Coinbase** (WS `snapshot`/`l2update`/`heartbeat` + strict `sequence == prev + 1`), **OKX** (WS `books` `seqId`/`prevSeqId` chaining + top-25 interleaved IEEE CRC32 verification) |
| **Normalization** | Exact fixed-point integer `PriceTicks` / `QtyLots` decimal parser (no FP drift), canonical instrument mapping (`BTCUSDT`/`BTC-USDT` -> `BTC/USDT`), tick/lot snapping, exchange + local receive timestamps |
| **Order books** | Cache-friendly `FlatOrderBook` (production hot path) + reference `MapOrderBook` (differential testing & benchmarking) |
| **Staleness & recovery** | Per-venue `BookState` FSM (`Disconnected`/`Connecting`/`Buffering`/`Synced`/`Stale`/`Resyncing`), configurable `max_stale_ns`, crossed-book guard, exponential reconnect backoff |
| **Scanner & cost model** | 6-route depth-aware marginal executable sizing & VWAP across L2 levels, per-venue taker fees, slippage buffer, pre-funded inventory caps, per-route cooldown, full rejection-reason telemetry |
| **Paper execution** | Strictly paper-only (zero credentials/order entry); pre-funded balances per venue; atomic reservations; Limit-IOC and Market-IOC modes; delayed legs executed against arrival-time *truth* books; non-instantaneous rebalance transfer fee/delay model; accounting invariant checker |
| **Replay & codecs** | Deterministic priority-queue replay engine with synthetic feed delay, order latency/jitter, and outage injection; JSONL and CRC32-validated binary (`XARB`) codecs; deterministic 3-venue synthetic feed generator; optional read-only live public recorder (`scripts/record_live_public.py`) |
| **Audit & analytics** | `summary.json`, `signals.csv`, `trades.csv`, indexed `audit.sqlite` (via SQLite3), deterministic FNV-1a `run_hash`, Python cross-check & latency-sweep analytics (`scripts/analyze_results.py`) |
| **Concurrency** | Lock-free cache-line-padded `SpscQueue` decoupling per-venue receiver threads from single-threaded engine consumer (verified under ThreadSanitizer) |

## Repository structure

```text
10-cross-exchange-arbitrage/
├── README.md
├── CMakeLists.txt
├── .clang-format
├── .gitignore
├── .github/workflows/ci.yml       # Build, Release/ASan+UBSan/TSan tests, determinism, analytics
├── include/xarb/                  # Public C++20 headers
├── src/                           # Core library and `xarb` CLI
├── tests/                         # Unit, differential, failure-mode, fuzz, and TSan tests
├── benchmarks/                    # FlatOrderBook vs MapOrderBook, E2E pipeline, codec benchmarks
├── examples/                      # Sample feeds and CLI usage guide
├── scripts/                       # Reproducible benchmark, demo, live-capture, and Python analytics
├── docs/                          # Design, assumptions, M5 results report, sample output
└── results/                       # Raw measured benchmark, replay, and latency-sweep outputs
```

## Implementation roadmap

- [x] M1 — Build verified public-feed adapters (Binance, Coinbase, OKX) and normalized fixed-point L2 book model.
- [x] M2 — Add snapshot/delta book consistency, gap/CRC32 recovery, stale-quote detection, and reconnect backoff.
- [x] M3 — Implement depth/fee-adjusted marginal VWAP scanner, risk/cost model, and pre-funded paper execution.
- [x] M4 — Introduce deterministic historical/recorded replay (JSONL & binary) and latency/outage stress sweeps.
- [x] M5 — Publish measured results report ([docs/results_report.md](docs/results_report.md)) including invalidated/false-positive signals and data-structure tradeoffs.

## Benchmarks and measured evidence

Full analysis in [docs/results_report.md](docs/results_report.md). Highlights from `./scripts/run_benchmarks.sh`:

- **Invalidated vs. actionable signals (5 ms Limit-IOC baseline):** Out of **24,314** gross top-of-book crosses across 29,577 events, **24,202 (99.54%)** were invalidated before execution (13,300 by taker fees/edge, 6,672 by pre-funded balance limits, 2,489 below min profit, 1,255 cooldown, 486 unsynced/stale). Of 112 actionable signals, 40 completed profitably while 55 degraded into one-legged or missed fills (`57.89%` false-positive rate).
- **Latency stress sweep (0 ms -> 100 ms):** Under Limit-IOC, false-positive rate rises from `0.00%` (0 ms) -> `57.89%` (5 ms) -> `84.72%` (10 ms) -> `100.00%` (50+ ms). Under Market-IOC, net paper P&L flips negative by **25 ms** (`-4,130.97 USDT`) and reaches `-16,290.52 USDT` at 100 ms (`90.36%` false-positive rate).
- **Data-structure tradeoff (`FlatOrderBook` vs `MapOrderBook`, 400k mixed ops):** At top-50 L2 depth, contiguous `FlatOrderBook` achieves **51.36 M ops/s** (p50 `18.2 ns`, p99 `26.1 ns`, p99.9 `88.6 ns`) vs `MapOrderBook` **37.61 M ops/s** (p50 `26.0 ns`, p99 `33.9 ns`, p99.9 `179.7 ns`) — **+36.6% throughput** and **2x lower p99.9 tail latency**.

### Benchmark reporting template

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores, ARM64, 16 GiB RAM |
| OS / kernel | macOS Darwin 25.5.0 (`RELEASE_ARM64_T8142`) |
| Compiler / version / flags | Apple Clang 21.0.0 (`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) |
| Dataset / event mix / sample count | Synthetic 3-venue wire feed (Binance + Coinbase + OKX JSON + CRC32): 44,577 E2E events; 400,000 book microbench ops (80% delta / 20% VWAP walk) |
| Warmup / affinity / governor | 2,000-event E2E warmup; 20,000-op book microbench warmup; macOS default scheduler (no pinning) |
| Throughput | **54,955 events/s** E2E (`54,937 book updates/s`); **51.36 M ops/s** `FlatOrderBook` (depth 50); **306,012 events/s** binary CRC32 decode |
| p50 / p99 / p99.9 latency | E2E per-event: **8.04 us / 26.75 us / 81.80 us**; `FlatOrderBook` (depth 50): **18.2 ns / 26.1 ns / 88.6 ns** |
| Raw results and reproduction command | [results/e2e_benchmark.json](results/e2e_benchmark.json), [results/book_tradeoff.csv](results/book_tradeoff.csv), [results/environment.txt](results/environment.txt) — `./scripts/run_benchmarks.sh` |

## Correctness and failure-mode tests

All 5 test suites pass under **Release (`-Werror`)**, **AddressSanitizer + UndefinedBehaviorSanitizer**, and **ThreadSanitizer**:

- `order_book_and_normalization`: fixed-point decimal parsing, symbol/lot normalization, 15,000-op differential test (`FlatOrderBook` vs `MapOrderBook`), depth-aware marginal cross sizing.
- `venue_adapters_and_recovery`: Binance delta buffering/bridge/stale-snapshot/gap recovery, Coinbase strict sequence & heartbeat liveness, OKX `prevSeqId` + IEEE CRC32 checksum corruption recovery, stale-quote transitions & exponential backoff.
- `scanner_risk_and_paper_sim`: venue outage suppression, insufficient depth notional capping, unfunded venue rejection, latency-induced one-legged fills, rebalance transfer fee/delay accounting invariants.
- `replay_determinism_and_fuzz`: byte-identical replay hash across repeated runs & JSONL vs binary feeds, latency-degradation monotonicity, 7,000+ fuzz inputs against binary frame decoder, JSON parser, and all 3 venue adapters.
- `spsc_concurrent_ingestion`: 3 concurrent venue-producer threads feeding 1 engine consumer via lock-free `SpscQueue` (verified race-free under TSan).

## Local development

```bash
# Release build + tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXARB_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# ASan + UBSan build + tests
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DXARB_WERROR=ON -DXARB_SANITIZE="address,undefined"
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer build + concurrency test
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DXARB_WERROR=ON -DXARB_SANITIZE="thread"
cmake --build build-tsan -j --target test_concurrency
ctest --test-dir build-tsan -R spsc_concurrent_ingestion --output-on-failure

# Full reproducible benchmarks, sweeps, and Python analytics cross-check
./scripts/run_benchmarks.sh
```

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset
- [x] Unit, integration, negative-case, fuzz/property, concurrency (TSan), and replay/stress tests
- [x] Reproducible benchmark script and machine/environment metadata
- [x] At least one measured performance tradeoff (`FlatOrderBook` vs `MapOrderBook` + binary vs JSONL codec), with raw results
- [x] Architecture diagram and explicit assumptions/limitations
- [x] Terminal demo and captured sample output showing the complete workflow
- [x] CI that builds, runs tests (Release, ASan+UBSan, TSan), checks formatting, and verifies replay determinism

## CV bullet

> Implemented a C++20 multi-exchange market-data normalizer and paper arbitrage engine (Binance, Coinbase, OKX) with venue-specific gap/CRC32 recovery, depth-aware marginal VWAP pricing, fee/latency modeling, and stale-quote safeguards; analyzed **24,314** replayed cross-venue signals across 0–100 ms latency stress sweeps.

## Caveats / non-goals

- Never call a detected cross-exchange spread risk-free arbitrage.
- Test on public data/paper balances; do not imply profitability without live execution evidence.
- Do not publish API secrets or exchange-specific restricted data.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- Official public WebSocket/REST market-data specifications for Binance Spot, Coinbase Exchange, and OKX v5
- *Trading and Exchanges* — Larry Harris (market structure)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
- C++20 memory model (`std::memory_order_acquire`/`release`) and compiler sanitizer documentation
