# Multi-Venue Smart Order Router & Execution Engine

> Route simulated orders across venues using liquidity, fees, latency, quote staleness, venue health, and pre-trade risk controls.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20, Python 3 (reporting), CMake, GoogleTest, zero-dependency JSON config  
**Status:** Implemented — builds cleanly with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, passes 22/22 unit/integration/property/fuzz tests under both Release and ASan+UBSan, with reproducible benchmarks and backtest experiments.

## Why build this?

Create an execution system that maintains a normalized consolidated L2 order book and makes explainable, auditable venue-selection decisions under realistic microstructure constraints (explicit maker/taker fees and rebates, stale market data, queue depletion during order flight, partial fills, venue outages, and pre-trade risk limits).

## Architecture

```text
Venue A feed --\
Venue B feed ---> QuoteNormalizer ---> ConsolidatedBook ---> RoutePlanner
Venue C feed --/    (wire decode,        (per-venue L2,       (4-term cost objective,
                     seq/ts checks)       staleness, BBO)      fee/latency/fill-prob)
                                                                     |
                                         RiskManager <-----> OrderManagementSystem
                                       (notional/pos/rate,    (parent/child FSM,
                                        collar, dedup)         cancel/replace, failover)
                                                                     |
                                                             VenueHealthMonitor &
                                                             JSONL AuditLog
```

See [docs/design.md](docs/design.md) for the full cost objective derivation, binary wire quote specification, and OMS state machine details, and [docs/assumptions.md](docs/assumptions.md) for explicit modeling assumptions and regulatory caveats.

## Implemented features

- **Per-venue L2 quote normalization & binary wire codec** (`QuoteNormalizer`, `encode_wire_quote`, `decode_wire_quote`): fixed-point `PriceTicks` (`$0.0001` scale), sequence-gap/out-of-order detection, negative-latency clamping, and FNV-1a checksummed binary wire frames.
- **Consolidated L2 book & staleness gating** (`ConsolidatedBook`): zero-allocation fixed-depth sorted arrays (`kMaxVenues = 8`, `kMaxBookDepth = 16`), raw and fee-adjusted effective BBO, and per-venue `stale_timeout_ns` exclusion.
- **Explainable cost-aware route planner** (`RoutePlanner`): decomposes every venue level into `raw_price_cost_bps + explicit_fee_bps + latency_risk_bps + miss_penalty_bps`, supports four policies (`naive_primary`, `naive_best_price`, `fee_aware`, `smart_cost_aware`), splits parent orders across venues respecting displayed size, `min_order_qty`, `max_child_qty`, `lot_size`, `max_active_children`, `max_parent_share`, `limit_price`, and `IOC`/`FOK`/`DAY`.
- **Parent/child OMS, cancel/replace & automatic failover** (`OrderManagementSystem`, `VenueHealthMonitor`): exact-once partial-fill accounting via `exec_id` deduplication, cancel/replace lifecycle, online EWMA latency & fill-ratio estimators, circuit breaker (`Healthy -> Degraded -> CircuitOpen`) blocking failing venues, and automatic remainder re-routing.
- **Pre-trade risk controls & structured audit log** (`RiskManager`, `AuditLog`): max order qty/notional, worst-case net & gross position limits including open working orders, sliding 1s order rate limit, price collar vs. consolidated mid, global kill switch, duplicate `client_order_id` rejection, and JSONL audit export.
- **Controlled discrete-event execution simulator** (`ExecutionSimulator`): separate true venue books vs. delayed router feeds, decoupled PRNG streams, competing-flow queue depletion with adverse level shift, partial fills, stale-feed freezes, and scripted venue outages across 5 controlled scenarios (`fee_inversion`, `stale_feed_trap`, `latency_race_depletion`, `venue_failover`, `mixed_stress`).

## Repository structure

```text
07-smart-order-router/
├── README.md
├── CMakeLists.txt
├── .clang-format
├── .github/workflows/ci.yml
├── include/sor/              # Public C++20 headers
├── src/                      # Core router, OMS, risk, simulator, CLI
├── tests/                    # Unit, integration, property, and fuzz tests (GoogleTest)
├── benchmarks/               # Router decision & wire-to-route latency benchmarks
├── examples/                 # Sample JSON venue & simulation configs
├── scripts/                  # Demo, format, benchmark, and Python experiment runners
├── docs/                     # Design, assumptions, benchmarks, experiments, sample output
└── results/                  # Reproducible raw benchmark & experiment outputs
```

## Benchmarks and measured evidence

Measured on this machine via `./scripts/run_benchmarks.sh` (see [docs/benchmarks.md](docs/benchmarks.md) and [results/benchmarks/router_latency.json](results/benchmarks/router_latency.json)):

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (10 physical / 10 logical cores, arm64) |
| OS / kernel | macOS Darwin 25.5.0 |
| Compiler / version / flags | Apple clang 21.0.0 (`-O3 -DNDEBUG -std=c++20`) |
| Dataset / event mix / sample count | 3 venues x 5 L2 levels/side, alternating Buy/Sell IOC (`100..1500` qty), 200,000 samples (`seed=42`) |
| Warmup / affinity / governor | 20,000 warmup ops; default macOS scheduling (no user-space governor or core pinning) |
| Throughput (`route_plan_only`, 3 venues) | **1,381,557 decisions/sec** (end-to-end `wire_decode_update_route`: **1,288,353 ops/sec**) |
| p50 / p99 / p99.9 latency (`route_plan_only`) | **640.6 ns / 1,533.9 ns / 8,984.4 ns** (`wire_decode_update_route`: **763.1 ns / 955.7 ns / 2,315.1 ns**) |
| Raw results and reproduction command | [results/benchmarks/router_latency.json](results/benchmarks/router_latency.json) — `./scripts/run_benchmarks.sh` |

### Controlled backtest & sensitivity highlights

Measured via `python3 scripts/run_experiments.py` (300 parent orders, `seed=42`, full tables in [docs/experiments.md](docs/experiments.md) and [results/experiments/summary.md](results/experiments/summary.md)):

- **Fee inversion (`fee_inversion`)**: `fee_aware` reduces net implementation shortfall from `0.47 bps` (`naive_best_price`) to **`0.36 bps`** (turning fees from `+0.12 bps` to `-0.02 bps` net rebate); at `2.0x` fee scale, `smart_cost_aware` saves **`0.21 bps`** over `naive_best_price`.
- **Latency race & queue depletion (`latency_race_depletion`)**: `smart_cost_aware` avoids the slow depleting venue (`0%` share), cutting cancel-to-fill ratio from **`0.49` to `0.09`** (5.4x lower) and median parent completion latency from **`4,514 us` to `880 us`** (5.1x faster), reaching `99.6%` fill ratio (`98.0%` vs. `90.4%` at `4.0x` latency scale).
- **Venue outage failover (`venue_failover`)**: `smart_cost_aware` trips the venue circuit breaker after 3 rejects (only `3` failovers vs. `130` for `naive_best_price`) and maintains **`99.2%` fill** (vs. `49.0%` for `naive_primary` and `93.8%` for `naive_best_price`).
- **Measured tradeoff (`mixed_stress`)**: `fee_aware` achieves lower net IS (`0.38 bps` vs. `0.46 bps`), while `smart_cost_aware` trades `+0.08 bps` for **2.0x faster median parent completion** (`443 us` vs. `885 us`), **2.0x lower cancel-to-fill** (`0.10` vs. `0.20`), and higher fill ratio (`99.3%` vs. `97.6%`).

## Correctness and failure-mode tests

All 22 tests in `tests/` pass in Release (`-Werror`) and under AddressSanitizer + UndefinedBehaviorSanitizer (`-DSOR_SANITIZE=ON`):

- `RoutePlannerTest.StaleBestAskDoesNotWinRouting` — stale quotes are excluded (`STALE_QUOTE`).
- `RoutePlannerTest.RespectsParentQtyLotSizeAndVenueChildLimits` — enforces `max_child_qty`, `min_order_qty`, `lot_size`, `max_active_children`, and `max_parent_share`.
- `OmsTest.PartialFillsReduceRemainingParentQtyExactlyOnce` — deduplicates redelivered `exec_id` fill reports.
- `RoutePlannerTest.HigherExchangeFeesReverseNaivePriceOnlyChoice` — fee-adjusted effective price reverses raw-price ranking.
- `OmsTest.FailingVenueCannotReceiveNewlyRoutedChildOrdersAndFailsOver` — circuit breaker isolates failing venue and fails over remaining quantity.
- `FuzzTest.BinaryWireQuoteDecoderNeverCrashesOnRandomOrMutatedInputs` — 10,000 randomized/mutated binary frames + exhaustive single-bit flip checksum verification.
- `PropertyTest.RoutePlannerInvariantsHoldAcrossRandomizedBooks` — 1,000 randomized multi-venue books verifying quantity conservation, staleness/health gating, lot/child limits, and limit-price bounds.

## Local development

```bash
# Build Release + run all 22 tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSOR_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# Build & test with AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSOR_SANITIZE=ON -DSOR_WERROR=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# Run end-to-end terminal demo (writes structured JSONL audit log)
./scripts/demo.sh

# Compare all routing policies on a scenario
./build/sor --compare --scenario mixed_stress --orders 300 --seed 42
./build/sor --simulate --config examples/fee_inversion.json

# Reproduce benchmarks and sensitivity experiments
./scripts/run_benchmarks.sh
python3 scripts/run_experiments.py
```

See [docs/sample_output.txt](docs/sample_output.txt) for captured output from `./scripts/demo.sh`.

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset ([docs/design.md](docs/design.md))
- [x] Unit, integration, negative-case and replay/stress/fuzz/property tests (`tests/`, 22/22 passing under Release and ASan+UBSan)
- [x] Reproducible benchmark script and machine/environment metadata (`scripts/run_benchmarks.sh`, `results/benchmarks/`)
- [x] At least one measured performance tradeoff, with raw results ([docs/experiments.md](docs/experiments.md), `results/experiments/`)
- [x] Architecture diagram and explicit assumptions/limitations ([docs/design.md](docs/design.md), [docs/assumptions.md](docs/assumptions.md))
- [x] Terminal demo and captured sample output showing the complete workflow (`scripts/demo.sh`, [docs/sample_output.txt](docs/sample_output.txt))
- [x] CI that builds (GCC Release + Clang ASan/UBSan), runs tests, smoke-tests demo, and checks formatting (`.github/workflows/ci.yml`)

## CV bullet

> Engineered a C++20 multi-venue smart order router with binary quote normalization, fee/latency/fill-probability-aware allocation, parent-child OMS with circuit-breaker failover, and pre-trade risk controls; measured **641 ns** median (**1.53 us** p99; **763 ns / 956 ns** end-to-end wire-decode-to-route) decision latency at **1.38M ops/sec** on Apple M5, cutting simulated cancel-to-fill ratios up to **5.4x** and net implementation shortfall up to **0.21 bps** vs. naïve price-only routing.

## Caveats / non-goals

- Do not confuse consolidated market data with guaranteed executable liquidity.
- Market impact and fill probability are models; see [docs/assumptions.md](docs/assumptions.md) for their limitations.
- Avoid claiming real NBBO compliance without a jurisdiction-specific implementation.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.
