# Ultra-Low-Latency Limit Order Book & Matching Engine

> A deterministic, price-time-priority exchange core written in modern C++.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20, CMake, GoogleTest, Google Benchmark, Linux perf (script provided, not yet run), sanitizers  
**Status:** Implemented (milestones M1–M4; M5 partially — see [Roadmap status](#roadmap-status)). Measured results are in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) and were taken on a busy laptop; read the caveats there before quoting them.

## Why build this?

Implement and measure the hot path of a simplified electronic exchange. This is an exchange matching engine, not a strategy or a broker.

**Hiring signal:** Demonstrates design tradeoffs, production-style correctness tests, and measurable engineering relevant to HFT software roles. A finished implementation with reproducible results is more persuasive than an ambitious design alone.

## Quick start

Requires CMake ≥ 3.20 and a C++20 compiler (tested with Apple clang 21). GoogleTest and Google Benchmark are downloaded by CMake if not installed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Replay the sample session (text in, text out) and print the final book
build/me_cli replay --text-in --text --symbols 2 --tick 5 --lot 10 --depth 5 \
    --in examples/sample_session.txt --out -

# Binary workflow: generate a command log, replay it with each book, compare the report streams
build/me_cli gen --out commands.bin --count 1000000 --seed 42 --symbols 4 --live 20000
build/me_cli replay --book pool --symbols 4 --in commands.bin --out reports.pool.bin
build/me_cli replay --book map  --symbols 4 --in commands.bin --out reports.map.bin
cmp reports.pool.bin reports.map.bin && build/me_cli dump --in reports.pool.bin | tail -3
```

`scripts/demo.sh` runs all of the above; its captured output is in [`docs/demo-output.txt`](docs/demo-output.txt).

```text
$ build/me_cli replay --text-in --text ... --in examples/sample_session.txt --out -
seq=7 sym=0 ACCEPTED id=7 side=S px=10000 qty=120
seq=8 sym=0 TRADE taker=7 maker=1 side=S px=10000 qty=100 taker_leaves=20 maker_leaves=0
seq=9 sym=0 TRADE taker=7 maker=2 side=S px=10000 qty=20 taker_leaves=0 maker_leaves=30
...
seq=18 sym=0 REPLACED id=9 new_id=10 side=B px=10015 qty=30 removed=50
seq=19 sym=0 TRADE taker=10 maker=6 side=B px=10015 qty=30 taker_leaves=0 maker_leaves=70
...
seq=23 sym=0 REJECTED id=20 reason=InvalidTick
```

## Architecture

```text
Binary/CLI replay -> command validator -> single-writer matching core
                                             |         |
                                             v         v
                                      order book   execution reports
                                             |
                                             v
                                       snapshot/metrics
```

The detailed diagram, component table, data-structure description and complexity table are in [`docs/DESIGN.md`](docs/DESIGN.md). The wire format is specified in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Supported feature subset

- Limit orders (buy/sell), FIFO time priority at each price level, partial and multi-level fills; trades execute at the resting order's price
- Cancel by order ID, quantity reduction (`Reduce`), and atomic cancel/replace (`Replace`)
- Multiple symbols with independent books and one engine-wide monotonic report sequence
- Deterministic execution report stream; top-of-book and N-level depth snapshots; engine counters
- Integer fixed-point prices and quantities with per-symbol tick size, lot size and maxima; no floating point anywhere in the engine
- Bounded order-ID mapping (`max_orders_per_book`), with a `BookFull` reject
- Two interchangeable books: `MapBook` (`std::map` + `std::list` + `std::unordered_map`) and `PoolBook` (preallocated order pool, intrusive FIFO, sorted level vectors, fixed-capacity open-addressing index)
- Fixed-size binary command (40 B) and report (64 B) records, plus a text format; streaming replay CLI

Not supported: market/IOC/FOK/stop/iceberg orders, auctions, self-trade prevention, accounts or risk checks, networking, persistence, a market-data feed, concurrency. The full list of assumptions is in [`docs/DESIGN.md`](docs/DESIGN.md#assumptions-and-limitations).

## Key engineering decisions

- **Single writer.** The engine is a single-threaded state machine with no locks, I/O or logging; reports leave through a caller-supplied sink. One engine per thread is the intended deployment.
- **Book layout.** A price level holds aggregate quantity and a FIFO queue of live orders; a separate order-ID index gives O(1) expected lookup.
- **Modification priority.** `Reduce` (quantity decrease) **retains** time priority. `Replace` **always loses** it, even at the same price and quantity: the old order is removed and the replacement queues as a new arrival. Price changes and quantity increases are only possible through `Replace`.
- **Deterministic validation.** Invalid quantities, prices, ticks, lots, sides, symbols, duplicate and unknown IDs are rejected in a documented order; a reject changes no state. `BookFull` is decided before matching so a refused order never trades.
- **Order IDs are unique per symbol**, not globally; every command carries its symbol.

## Repository structure

```text
01-matching-engine/
├── README.md
├── CMakeLists.txt
├── include/me/        # types, protocol, engine, map_book, pool_book (header-only core) + support headers
├── src/               # text format, workload generator, allocation counter, me_cli
├── tests/             # scenario, validation, differential, protocol/replay/fuzz, allocation, CLI tests
├── benchmarks/        # me_bench (Google Benchmark), me_latency (percentiles/throughput/allocations)
├── examples/          # sample session and its expected reports (golden file)
├── scripts/           # run_benchmarks.sh, summarize.py, perf_stat.sh, demo.sh, check_format.sh
├── docs/              # DESIGN.md, PROTOCOL.md, BENCHMARKS.md, demo-output.txt
├── results/           # raw benchmark output (generated, not fabricated)
└── .github/workflows/ # CI definition
```

## Roadmap status

1. **M1 — done.** Reference implementation using `std::map` and linked per-price queues (`MapBook`).
2. **M2 — done.** Invariant checks on both books and randomized differential tests against an independently written naive reference engine (`tests/reference_engine.hpp`).
3. **M3 — done.** Order pool, intrusive queues, cache-conscious level and index structures (`PoolBook`); hot-path allocation instrumentation (`me_alloc_counter`).
4. **M4 — done.** Streaming binary commands and execution reports (`me_cli`), reproducible benchmark script.
5. **M5 — partial.** `MapBook` vs `PoolBook` is measured and written up, including one case where the optimized book loses. No profiler traces or hardware-counter data were collected, and no optimization beyond M3 was made.

## Benchmarks

One run, Apple M5 laptop, macOS 26.5.1, Apple clang 21 `-O3`, on battery with other CPU-heavy processes running and no core pinning. Run-to-run variation was large. Full tables, method and caveats: [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md). Raw output: [`results/20261124T013847Z/`](results/20261124T013847Z/).

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency), 16 GB RAM |
| OS / kernel | macOS 26.5.1 (25F80), Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0, `-std=c++20 -O3 -DNDEBUG` |
| Dataset / event mix / sample count | Synthetic, seed 1; 2,000,000 measured commands × 3 runs; 45% new orders, 34% cancels, 4% reduces, 4% replaces, 13% rejects; 210,646 trades |
| Warmup / affinity / governor | 500,000 commands; no affinity (macOS); no governor control; battery power; machine not idle |
| Throughput | `PoolBook` 22.5 M commands/s (range 19.8–23.0); `MapBook` 9.6 M commands/s (9.6–11.0). 1 symbol, ~7,600 resting orders, in-process, no I/O |
| p50 / p99 / p99.9 latency | `PoolBook` 42 / 84 / 208 ns; `MapBook` 83 / 250 / 500 ns per command. Clock granularity 41 ns, each sample includes a ~24 ns clock read |
| Raw results and reproduction command | `results/20261124T013847Z/`; `scripts/run_benchmarks.sh` |

What was measured, in short:

- **Allocations:** `PoolBook` made 0 heap allocations in the measured window of all 30 runs; `MapBook` made 0.91–0.95 per command.
- **Mixed-workload throughput:** `PoolBook` was 1.5x–2.3x `MapBook` in eight right-sized configurations (4.1x in a ninth, noisy one).
- **Measured tradeoff:** over-provisioning hurts the pooled book. With 256 symbols and the same ~78k resting orders, raising per-book capacity from 2,584 to 200,000 dropped `PoolBook` from 11.9 to 3.5 M commands/s and raised p99 from 250 ns to 2,084 ns — about half of `MapBook`'s 6.8 M/s. The cause was not profiled; the capacity-sized order-ID index is the likely one.
- **Sensitivity:** `PoolBook` throughput went 28.1 → 22.5 → 14.7 → 8.4 M commands/s from ~0.8k to ~770k resting orders; random 64-bit order IDs cost about a third at ~770k orders and nothing visible at ~7.6k.
- **Unresolved:** passive-add on books of ≥100k orders, where the two books were within noise of each other.
- **Not measured:** LLC misses, branch misses and CPU cycles (they need Linux `perf`; `scripts/perf_stat.sh` exists but has not been run).

```bash
scripts/run_benchmarks.sh          # full run (~5 min) -> results/<UTC timestamp>/
scripts/run_benchmarks.sh --quick  # smoke test
```

Do not insert example numbers as if observed. Keep methodology, source code revision, and command-line parameters alongside any published measurements.

## Correctness and failure-mode tests

116 tests (`ctest`), all passing in Release and in a Debug build with AddressSanitizer + UndefinedBehaviorSanitizer:

| README requirement | Test |
|---|---|
| Two equal-price buys execute in arrival order against one incoming sell | `Matching.EqualPriceBuysFillInArrivalOrder` |
| A sell crossing three bid levels executes best price first, then FIFO within level | `Matching.SellSweepsThreeBidLevelsBestPriceFirstThenFifo` |
| A cancellation removes remaining quantity, never previously filled quantity | `Matching.CancelRemovesOnlyTheUnfilledRemainder` |
| A quantity reduction preserves priority per chosen rule | `Matching.ReduceKeepsTimePriority`, `Matching.ReplaceLosesTimePriorityEvenAtSamePrice` |
| Zero/negative quantities, duplicated IDs, and invalid ticks are rejected | `Validation.*` (12 cases × 2 books) |
| Replay of a fixed command log produces byte-identical reports | `Replay.FixedCommandLogProducesByteIdenticalReports`, `GoldenReplay.*`, `cli_replay_is_byte_identical` |

Also:

- **Differential tests:** 8 workload scenarios × 4 seeds, 15–20k commands each including malformed ones; both books must match the naive reference engine report for report, with structural invariants and depth snapshots checked every 64 commands.
- **Binary-input fuzzing:** `Fuzz.RandomBinaryCommandsNeverBreakTheEngine` feeds 240,000 random and semi-random 40-byte records through both books and the reference. A libFuzzer harness (`tests/fuzz_engine.cpp`, `-DME_BUILD_FUZZERS=ON`) is provided; it compiles but has **not been run**, because Apple clang ships no libFuzzer runtime.
- **Allocation tests:** `PoolBook` processes 200,000 mixed commands with zero heap allocations.
- **Mutation check:** two bugs injected by hand into `PoolBook` (hash-index wraparound, level aggregate) were each caught by the suite.
- ThreadSanitizer is not used: there is no concurrent code.

```bash
# Sanitizer build
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DME_SANITIZE=address,undefined -DME_WERROR=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

scripts/check_format.sh        # clang-format check (--fix to apply)
```

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset
- [x] Unit, integration, negative-case and replay/stress tests
- [x] Reproducible benchmark script and machine/environment metadata
- [x] At least one measured performance tradeoff, with raw results
- [x] Architecture diagram and explicit assumptions/limitations
- [x] Short video, terminal demo or sample output showing the complete workflow ([`docs/demo-output.txt`](docs/demo-output.txt))
- [ ] CI that builds, runs tests and checks formatting/linting — the workflow is written (`.github/workflows/ci.yml`) but has **never run**: this directory is not yet a git repository. The same steps pass locally on macOS; the Linux/GCC legs are untested.

## CV bullet

Filled in from the run above. Re-measure on an idle, pinned machine before using it; these numbers came from a loaded laptop.

> Implemented a deterministic C++20 price-time-priority matching engine with multi-level partial fills, order cancellation, and preallocated order storage; benchmarked **22.5 M** events/sec and **84 ns** p99 event latency (41 ns timer granularity) on **Apple M5 / macOS 26.5 / Apple clang 21**, with zero heap allocations per event.

## Caveats / non-goals

- Do not call an average or p50 “ultra-low latency”; publish tail distributions.
- Do not claim allocation-free matching until counters confirm no allocations on the measured path. (Confirmed for `PoolBook` with fewer than 4,096 price levels per side; beyond that the level vector can grow.)
- Avoid using floating-point prices in core order matching.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- Official protocol specifications for whichever feed/order-entry version you implement
- *Trading and Exchanges* — Larry Harris (market structure)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
- Linux `perf` documentation, compiler documentation, and C++ memory-model references
