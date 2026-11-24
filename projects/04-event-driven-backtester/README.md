# Event-Driven, Order-Book-Aware HFT Backtester

> A deterministic simulation engine that models events, queue position, fills and latency.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20 engine, Python tooling (pandas, Arrow/Parquet), CMake, GoogleTest, pytest  
**Status:** Implemented (roadmap M1–M5). All published numbers are measured on **synthetic** order
flow on one laptop; none of them is a claim about real markets. See
[Caveats](#caveats--non-goals).

## What it does

Replays a historical L2 or L3 feed through a simulated exchange, lets a strategy observe that feed
*late*, send orders that arrive *late*, and learn about its fills *late* — and reports what would
have happened under explicit queue-position, latency and fee assumptions.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/hftbt run --config examples/configs/market_maker.ini
```

```text
feed             examples/data/sample_l3.csv (20000 events, 1 instruments)
strategy         market_maker
simulated span   33.847 s
dispatched       54785 events in 0.002 s wall (25.14 M events/s)
orders           sent 3099, risk-rejected 0, exch-rejected 3, cancels 3065 (416 too late)
fills            534 (534 maker / 0 taker), fill ratio 0.1480
pnl net / gross  5628.46 / 5172.00 (fees -456.46)
max drawdown     118.01
markout 100000000 maker +1.1467 ticks/lot (4565 lots), taker +0.0000 (0 lots)
event hash       90de3f2736369387
```

The event hash is a digest of every dispatched event and the final account state: run it again and
you get the same 16 hex digits. Full workflow output is in
[docs/sample_output.txt](docs/sample_output.txt) (`scripts/demo.sh`).

## Architecture

```text
Historical feed -> event scheduler -> simulated exchange (truth book, queue positions)
                         |                    |
                         |          market-data latency
                         v                    v
                     strategy <------ observed book (delayed copy)
                         |
                   OMS / pre-trade risk   (position from observed fills only)
                         |
              compute delay + order-path latency
                         |
                 execution model -> portfolio -> reports
                         |
                 response latency -> strategy
```

Details, event-ordering rules, the fill model and file formats: [docs/design.md](docs/design.md).

## Supported feature set

| Area | Implemented |
|---|---|
| Time | `int64` nanosecond timestamps end to end |
| Event ordering | Total order by (timestamp, event class, sequence); feed, order-entry, observed-data, response, timer, markout and sampling events |
| Feeds | L3 market-by-order (add / cancel / delete / execute) and L2 market-by-price (level / trade); CSV and a validated binary format; multiple instruments on one clock |
| Fill model | Exact FIFO queue position on L3; three explicit queue models on L2 (conservative, proportional, optimistic); partial fills; trade-through and crossing-quote fills; taker fills walking displayed depth |
| Orders | Limit GTC / IOC, market, post-only, cancel; cancel/fill races resolved by arrival time |
| Latency | Independent market-data, order-entry and response delays with seeded jitter, plus strategy compute delay; each path is ordered |
| Costs | Maker / taker fees in bp (rebates allowed), fixed taker slippage |
| Risk | Max order size, worst-case position limit including open orders, max open orders — checked locally before an order is sent |
| Accounting | Cash + mark-to-market inventory, cross-checked against realized/unrealized average-cost P&L |
| Reports | P&L, fees, turnover, fill ratio, drawdown, exposure, adverse-selection markouts at configurable horizons; `summary.json`, `fills.csv`, `equity.csv`, optional event trace |
| Strategies | Inventory-skewed market maker, top-of-book imbalance taker (baselines, not recommendations) |
| Tooling | Synthetic L3 generator, L3→L2 converter, Parquet↔CSV conversion, report cross-check, parameter sweep with in-sample / out-of-sample split |

Not supported: order modify/replace, hidden liquidity, market impact, auctions, native exchange
protocols. The full list with the direction of each bias is in
[docs/assumptions.md](docs/assumptions.md).

## Key engineering decisions

- **Causality is structural.** There are two books per instrument: exchange truth and the delayed
  copy the strategy sees. The strategy API exposes only the delayed one, and only the position
  implied by fills it has already been told about.
- **Queue position is explicit.** On L3 data it is exact and verified against a brute-force model;
  on L2 data the unknown is a named configuration choice, and the experiments show how much it
  moves the result.
- **Ties go to the market.** At equal timestamps historical events are applied before simulated
  orders.
- **Races are races.** A cancel that arrives after the fill is rejected as too late; the fill
  stands.
- **Bad input is an error, not a guess.** Malformed files are rejected with a line number;
  inconsistent events are skipped and counted; unknown config keys abort the run.
- **Historical profitability is a hypothesis.** Sweeps report in-sample and out-of-sample
  side by side with the number of trials.

## Repository structure

```text
04-event-driven-backtester/
├── CMakeLists.txt
├── include/hftbt/        # public headers: engine, exchange, order_book, strategy API, ...
├── src/                  # implementation + CLI (main.cpp)
├── tests/                # GoogleTest: unit, causality, determinism, fuzz/property
│   └── python/           # pytest: CLI end-to-end + Python tooling
├── benchmarks/           # bench_replay.cpp
├── python/hftbt_tools/   # feedio (Parquet), report, sweep
├── examples/             # configs and small reproducible fixtures
├── scripts/              # demo, benchmarks, experiments, mutation check, formatting
├── docs/                 # design, assumptions, benchmarks, experiments, sample output
└── results/              # raw benchmark / experiment output (generated, never hand-edited)
```

## Build, test, run

Requires CMake ≥ 3.20 and a C++20 compiler (tested: Apple clang 21). GoogleTest is fetched at
configure time. Python 3.9+ is needed only for the tooling and the end-to-end tests.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure            # 65 C++ tests

pip install -r requirements.txt
python -m pytest tests/python -q                      # 11 end-to-end / tooling tests

# AddressSanitizer + UBSan, warnings as errors
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHFTBT_SANITIZE=ON -DHFTBT_WERROR=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

scripts/demo.sh                                       # whole workflow on bundled fixtures
```

CLI:

```bash
build/hftbt gen --out feed.bin --events 1000000 --seed 7 [--instruments 4] [--l2]
build/hftbt convert feed.bin feed_l2.csv --to-l2
build/hftbt info feed.bin
build/hftbt run --config examples/configs/market_maker.ini \
    --set latency.order_delay_ns=1000000 --set fees.taker_bps=0.5 [--json | --quiet]

PYTHONPATH=python python -m hftbt_tools.feedio to-csv ticks.parquet feed.csv --tick-size 0.01
PYTHONPATH=python python -m hftbt_tools.report results/sample_market_maker
PYTHONPATH=python python -m hftbt_tools.sweep --config examples/configs/market_maker.ini \
    --grid strategy.half_spread_ticks=1,2,3 --grid latency.order_delay_ns=0,100000,1000000 \
    --out results/sweep_mm
```

Configuration reference by example: [examples/configs/market_maker.ini](examples/configs/market_maker.ini).
Writing a strategy means subclassing `hftbt::Strategy`
([include/hftbt/strategy.hpp](include/hftbt/strategy.hpp)); the baselines in
[src/strategies.cpp](src/strategies.cpp) are short enough to read as examples.

## Correctness and failure-mode tests

| Requirement | Test |
|---|---|
| A strategy cannot trade on an event before it is observed | `Causality.StrategyCannotSeeAnEventBeforeItsObservableTime` — compares truth and observed books at specific nanoseconds |
| No fill if the order has not reached the simulated exchange | `Causality.NoFillBeforeTheOrderReachesTheExchange`, `Causality.ComputeDelayIsAddedToTheOrderPath`, and an assertion on every report in the fuzz test |
| Cancel arriving after a fill is a race, not retroactive removal | `Causality.CancelArrivingAfterFillIsARaceNotARetroactiveRemoval` and its mirror `CancelThatWinsTheRacePreventsTheFill` |
| Cash + marked inventory equals tracked equity | `Accounting.CashPlusMarkedInventoryEqualsTrackedEquity` — two conventions plus a from-scratch replay of the fill tape; also on fuzzed input |
| Same input/seed/config gives identical events and P&L | `Determinism.*` (bitwise), `Scheduler.FifoAndHeapMarketDataQueuesAreEquivalent`, and byte-identical report files in `tests/python` |
| Queue model is right | `Property.L3QueuePositionMatchesBruteForceFifoModel` — 300 random scenarios against an independent model |
| Binary/CSV input cannot crash the loader | `Fuzz.BinaryParserSurvivesRandomAndMutatedInput`, `Fuzz.CsvParserSurvivesMutatedInput` (20,000 mutated inputs each) |
| Engine invariants under hostile data | `Fuzz.EngineInvariantsHoldOnHostileFeedsWithChaoticStrategy` — inconsistent feeds, random strategy, random latency configs |

The suite passes under ASan + UBSan with `-Werror`. The engine is single-threaded, so
ThreadSanitizer does not apply.

To check the tests have teeth, `scripts/mutation_check.py` injects 19 deliberate bugs one at a time
(ignored delays, strategy reading the truth book, skipped queue priority, flipped fee sign, swapped
tie-break, ...) and re-runs the suite. 18 are caught. The survivor — swapping the tie-break between
"order arrives" and "market data observed" within one nanosecond — is an equivalent mutant: neither
the strategy nor the exchange can observe the difference. The first pass of this check found one
real gap (the feed-before-orders tie-break was not pinned down), which is now covered by
`Causality.FeedEventsAtATimestampPrecedeOrdersArrivingAtThatTimestamp`.

## Benchmarks

Apple M5, macOS 26.5.1, Apple clang 21 `-O3`, 2,000,000 synthetic L3 events per scenario, best of
7 runs. Environment, methodology, noise floor and all tables: [docs/benchmarks.md](docs/benchmarks.md).
Raw output: [results/benchmarks/](results/benchmarks/). Reproduce: `scripts/run_benchmarks.sh 2000000 7`.

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency), 16 GB |
| OS / kernel | macOS 26.5.1, Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0, `-O3 -DNDEBUG -std=c++20` |
| Dataset / event mix / sample count | Seeded synthetic L3 flow (≈42% add, 36% cancel, 22% execute), 2,000,000 events |
| Warmup / affinity / governor | None / not pinned (no macOS API) / n/a, on battery |
| Throughput | 23.9 M dispatched events/s (9.8 M feed events/s) with the market-making strategy; 30.2 M/s with a no-op strategy |
| p50 / p99 / p99.9 latency | 42 / 84 / 166 ns per dispatched event, quantised to a 41 ns clock tick |
| Raw results and reproduction command | `results/benchmarks/`, `scripts/run_benchmarks.sh 2000000 7` |

Headline findings:

- **Measured tradeoff — FIFO vs heap for in-flight market data.** Both give identical results. The
  FIFO is flat in the latency setting; the heap is 9% slower with ~1 event in flight and 42% slower
  with ~10,000 (500 ms delay).
- **Instruments:** 1 → 256 instruments on the same event count costs 34% of throughput (working-set
  growth, not algorithmic).
- **Active orders:** 16 resting orders cost about 5%; 1,024 inside the traded range cost 60%,
  because simulated orders are scanned linearly by price.
- **Length:** throughput is flat from 1 M to 64 M feed events (156.9 M dispatched in 6.9 s), with
  identical hashes on repeat. Memory is 40 bytes per feed event; the feed is held in RAM.

## Strategy experiments

Five synthetic sessions, reproducible with `python scripts/run_experiments.py`; write-up in
[docs/experiments.md](docs/experiments.md), raw rows in [results/experiments/](results/experiments/).

- **Latency:** the market maker's net P&L falls monotonically from 102k (0 latency) to 79k (100 µs)
  to 23k (1 ms) and turns negative at 10 ms, where its markouts go negative too.
- **Fees:** behaviour and gross P&L are unchanged; only the fee line moves.
- **Queue model:** on the same flow reduced to L2, fills order as conservative < exact L3 <
  optimistic, about ±1% apart for this strategy.
- **In-sample vs out-of-sample:** rank correlation is ≈0.9–0.99 — because the generator is
  stationary. This validates the tooling, not the strategy.

The imbalance taker loses money at every setting, which is the correct answer on flow where trade
direction is a coin flip.

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset
- [x] Unit, integration, negative-case and replay/stress tests
- [x] Reproducible benchmark script and machine/environment metadata
- [x] At least one measured performance tradeoff, with raw results
- [x] Architecture diagram and explicit assumptions/limitations
- [x] Sample output showing the complete workflow ([docs/sample_output.txt](docs/sample_output.txt))
- [ ] CI that builds, runs tests and checks formatting — the workflow is written
  ([.github/workflows/ci.yml](.github/workflows/ci.yml)) but has never run: this directory is not
  yet a git repository. Everything it runs has been run locally on macOS; Linux is untested.

## CV bullet (still not usable as written)

> Developed a deterministic C++ event-driven trading simulator incorporating market-data/entry latency, partial fills, queue-position assumptions and transaction costs; evaluated market-making strategies across **[number]** historical sessions with sensitivity analysis.

The simulator half is done. The second half says *historical* sessions; so far it has only seen
synthetic ones. Fill in the number after running it on real order-level data, or reword to
"synthetic sessions".

## Caveats / non-goals

- A backtest against only OHLC candles is not an HFT event simulator.
- Never claim exact historical fills without sufficient order-level data. On L2 data this simulator
  does not either — it makes you choose a queue model.
- Avoid presenting in-sample Sharpe as out-of-sample evidence. No Sharpe ratio is reported at all.
- The bundled data is synthetic and uninformed: a market maker earning the spread on it is a
  property of the generator.
- Results from one unpinned laptop on battery are indicative. Linux/x86 and real feeds are
  unmeasured.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- Official protocol specifications for whichever feed/order-entry version you implement
- *Trading and Exchanges* — Larry Harris (market structure)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
- Linux `perf` documentation, compiler documentation, and C++ memory-model references
