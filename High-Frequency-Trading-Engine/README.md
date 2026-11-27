# High-Frequency Trading Engine

> A C++20 simulated high-frequency trading platform composing ten standalone market-data, matching, order-entry, transport, routing, and strategy modules into a single deterministic, fault-tested, and benchmarked engine.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20, CMake, POSIX TCP/UDP loopback sockets, lock-free SPSC queues, sanitizers (ASan/UBSan/TSan), Python 3  
**Status:** Implemented and verified on macOS/arm64 (Release `-Werror`, ASan+UBSan, TSan, and live loopback TCP/UDP multicast). The Linux CI workflow is configured and ready to run on GitHub Actions. Benchmarks below were measured on an Apple M5 laptop; read [`docs/performance-report.md`](docs/performance-report.md) before quoting them.

This repository contains the integration layer (`engine/`), versioned contracts (`contracts/v1`), fault/golden tests, scripts, benchmarks, and documentation. It compiles directly against the ten independently runnable component libraries in [High-Frequency-Trading](https://github.com/vaibhavkapur/High-Frequency-Trading) without duplicating their source trees. It is an educational research simulator, not a live exchange integration or demonstrated profitable trading strategy.

## Getting started

Requires CMake 3.20+ and a C++20 compiler. CMake automatically locates a local checkout of [`vaibhavkapur/High-Frequency-Trading`](https://github.com/vaibhavkapur/High-Frequency-Trading) (via `-DHFT_PROJECTS_ROOT=...`, `$HFT_PROJECTS_ROOT`, or a sibling `../High-Frequency-Trading/projects` folder) or fetches the pinned commit automatically when configuring.

```bash
git clone https://github.com/vaibhavkapur/High-Frequency-Trading-Engine.git
cd High-Frequency-Trading-Engine

cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine --parallel
ctest --test-dir build/engine --output-on-failure
```

To build completely offline against an existing local clone of `High-Frequency-Trading`:

```bash
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release \
  -DHFT_PROJECTS_ROOT=/path/to/High-Frequency-Trading/projects
cmake --build build/engine --parallel
ctest --test-dir build/engine --output-on-failure
```

Run the single-venue golden check or the full walkthrough:

```bash
# Verify single-venue deterministic replay against the frozen golden fixture
./build/engine/hft_demo --mode deterministic --scenario single-venue \
  --verify-golden engine/tests/golden/single_venue.golden.json

# Run all deterministic, network-lab, and performance scenarios
./scripts/run_demo.sh

# Include live loopback TCP (OUCH Gateway <-> PoolBook ExchangeSim) and UDP multicast recovery
LIVE_SOCKETS=1 ./scripts/run_demo.sh

# Reproduce stage-level and end-to-end benchmarks with raw CSV/JSON artifacts
./scripts/run_benchmarks.sh integration/results/latest
```

## Architecture

```text
  Historical ITCH 5.0 / Synthetic L3 Feed (02, 04)
                       |
                       v
            Normalized MarketDataEvent (FeedOrderRef)
               |                              |
               v                              v
   Authoritative Venue Book (01)    MDF1 Multicast + Recovery (06)
     me::Engine<PoolBook|MapBook>             |
               |                     Direct / Bounded SPSC (05)
        Causal Latency (04)                   |
               +---------------+--------------+
                               |
                               v
             Observed Canonical Book (Staleness + Sync Gate)
                               |
                               v
         Strategy Plugin: Deterministic / Market Maker (08) / Paper Arb (10)
                               |
                               v
              Pre-Trade Risk Gate & Kill Switch (Worst-Case Exposure)
                               |
                               v
                    Smart Order Router (07, optional)
                               |
                               v
            OUCH-Style Gateway / OMS Reconciler / Binary EventLog (03)
                               |
                               v
         Fixed-Point Accounting Ledger ($0.0001) + Perf Lab (09)
```

Detailed Mermaid architecture, sequence, and module-dependency diagrams plus Architecture Decision Records (ADRs 001–005) are in [`docs/architecture.md`](docs/architecture.md). Canonical type scales, clock-domain isolation (`SimTimeNs` vs `WallTimeNs`), and overflow contracts are specified in [`docs/contracts.md`](docs/contracts.md).

## Upstream Modules Integrated

The ten standalone projects live in [`vaibhavkapur/High-Frequency-Trading`](https://github.com/vaibhavkapur/High-Frequency-Trading) (pinned baseline commit `8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89`) and are wrapped through thin adapters in `engine/src/adapters/`:

- [01 · Matching Engine](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/01-matching-engine) — Authoritative price-time matching (`PoolBook` and `MapBook`) via `matching_adapter`, plus `OuchMatchingBridge` for the TCP exchange simulator.
- [02 · ITCH Feed Handler](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/02-itch-feed-handler) — Nasdaq ITCH 5.0 decode and reference BBO verification via `itch_adapter`.
- [03 · OUCH Order Gateway](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/03-ouch-order-gateway) — OUCH-style wire codec, binary `EventLog` replay, and loopback SoupBinTCP session via `ouch_adapter`.
- [04 · Event-Driven Backtester](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/04-event-driven-backtester) — Monotonic FIFO `LatencyPath` and synthetic L3 feeds via `backtester_adapter`.
- [05 · Lock-Free Messaging](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/05-lock-free-messaging) — Bounded cache-line-padded `lfm::SpscQueue` with explicit overflow policies via `spsc_adapter`.
- [06 · UDP Multicast Feed](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/06-udp-multicast-feed) — Custom `MDF1/MDR1` multicast dissemination, fault injection, and retransmit/snapshot recovery via `multicast_adapter`.
- [07 · Smart Order Router](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/07-smart-order-router) — Per-instrument multi-venue quote aggregation, cost/fee/staleness-aware routing, and failover via `router_adapter`.
- [08 · Market-Making Simulator](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/08-market-making-simulator) — Observed-only order-book features and inventory-aware quoting plugin via `strategy_adapter`.
- [09 · Latency Performance Lab](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/09-latency-performance-lab) — Wall-clock percentile harness, environment provenance, and CSV exporters via `perf_adapter`.
- [10 · Cross-Exchange Arbitrage](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/10-cross-exchange-arbitrage) — Paper-only multi-venue arbitrage scanner and delayed-leg execution simulator via `arb_adapter`.

## Repository Layout

```text
High-Frequency-Trading-Engine/
├── README.md
├── plan.md                             # Integration roadmap and acceptance criteria
├── engine/
│   ├── CMakeLists.txt
│   ├── cmake/UpstreamModules.cmake     # Isolated upstream targets (local or FetchContent)
│   ├── include/hft/contracts/v1/       # Versioned fixed-point types, events, wire codec, interfaces
│   ├── include/hft/adapters/           # Thin adapter interfaces wrapping modules 01–10
│   ├── include/hft/runtime/            # Canonical book, scheduler, risk, ledger, OMS, faults, pipeline
│   ├── src/adapters/                   # Adapter implementations (01–10)
│   ├── src/runtime/                    # Runtime orchestration implementations
│   ├── apps/                           # hft_demo and hft_bench CLIs
│   ├── config/default.json             # Default instruments, venues, and risk limits
│   └── tests/                          # Contract, differential, E2E, failure, multicast/SPSC, strategy, golden
├── integration/
│   ├── scenarios/scenarios.json        # Scenario manifest
│   └── results/apple_m5_release/       # Raw benchmark CSV/JSON, provenance, and generated summary
├── scripts/                            # build_all.sh, run_demo.sh, run_benchmarks.sh, summarize_benchmarks.py
├── docs/                               # integration-audit, contracts, architecture, failure-modes, performance-report, showcase
└── .github/workflows/ci.yml            # Multi-compiler Release, ASan+UBSan, TSan, and non-gating perf CI
```

## Tests

All 6 integrated CTest suites (17 test cases) pass on Apple M5 / macOS 26.5.1 in three configurations: Release (`-DHFT_WERROR=ON`), AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer.

| Suite | Cases | Coverage |
|---|---:|---|
| `test_contracts` | 3 | Fixed-point tick/lot/Money overflow, clock-domain isolation, `"HFT1"` wire round-trip & version mismatch, ITCH/OUCH protocol boundary isolation |
| `test_matching_differential` | 2 | `PoolBook` vs `MapBook` differential equivalence across GTC/IOC/Market/Post-Only/Cancel/Replace; `OuchMatchingBridge` on Project 01 `PoolBook` |
| `test_deterministic_e2e` | 2 | Bit-identical single-venue replay against `single_venue.golden.json` (`66d95957646657a6`), zero causality violations, ITCH stream BBO match vs reference book |
| `test_failure_recovery` | 4 | Duplicate `ExecId` idempotence, disconnect `Uncertain` exposure retention, cancel/fill race, Project 03 `EventLog` replay, risk negative cases & kill switch, exact integer ledger conservation through position flips |
| `test_multicast_spsc` | 3 | `MDF1` recovery under drops/corruption/duplicates/reordering, snapshot fallback + out-of-sync trading block, SPSC `Backpressure`/`Reject`/`DropAndResync` policies (never dropping orders/execs) |
| `test_multi_venue_strategies` | 3 | Multi-venue SOR fee-aware routing, staleness exclusion & failover, Project 08 `MarketMakerPlugin` inventory skew & reconciliation, Project 10 paper-arbitrage negative cases & one-legged latency risk |

Live loopback socket verification (`LIVE_SOCKETS=1 ./scripts/run_demo.sh`) also passes: `MDF1` UDP multicast recovers injected drops over TCP (`gaps=1, recovered=3, converged=YES`), and the OUCH SoupBinTCP gateway connects to `ExchangeSim` (backed by Project 01 `PoolBook`), matches orders, recovers from a forced disconnect, cancels remaining depth, and passes OMS invariants.

## Benchmarks

Measured with `hft_bench` (`integration/results/apple_m5_release/`). Full stage breakdown, comparative investigations (`PoolBook` vs `MapBook`, direct vs SPSC, clean vs 15%-loss multicast), and methodology caveats are in [`docs/performance-report.md`](docs/performance-report.md).

| Field | Value |
|---|---|
| CPU / cores / architecture | Apple M5, 10 logical cores (`arm64`) |
| OS / kernel | macOS 26.5.1, Darwin 25.5.0 |
| Compiler / flags | AppleClang 21.0.0 (`Release`, `-O3 -DNDEBUG`, `-std=c++20`) |
| Clock & sampling | `WallTimeNs` (`steady_clock`, overhead 19.17 ns); 2 warmup rounds + 5 reps × 20 samples (`500` ops/sample; `10,000` for steady-state matching); 100 warmup + 500 ops for E2E histograms |
| Steady-state matching A/B | `PoolBook` **16.24 ns** p50 / 27.99 ns p99 (**61.58 Mops/s**) vs `MapBook` 34.12 ns p50 / 49.68 ns p99 (29.31 Mops/s) — **2.10× speedup**, identical checksums |
| Feed transport compare | Direct in-process **15.75 ns** p50 vs two-thread `lfm::SpscQueue` **54.46 ns** p50 (0 drops, identical book hash) |
| Multicast recovery overhead | Clean `MDF1` **253.12 ns** p50 vs 15%-drop + 10%-reorder recovered **310.75 ns** p50 (+22.8%, identical state hash) |
| End-to-end hot-path distributions | `tick_to_decision` p50 **333 ns** / p99 **1,125 ns** · `order_submit_to_ack` p50 **125 ns** / p99 **791 ns** · `order_submit_to_fill` p50 **166.5 ns** / p99 **876 ns** |

## Documentation

- [Integration Audit & Baseline Inventory](docs/integration-audit.md) — Phase 0 toolchain provenance, standalone build/test matrix, collision inventory, and ADRs.
- [Canonical Contracts (`contracts/v1`)](docs/contracts.md) — Fixed-point scales, clock domains, ID separation, wire format, and queue overflow rules.
- [Architecture & ADRs](docs/architecture.md) — Reference architecture, causality sequence, module dependency graph, and state ownership.
- [Failure Modes & Recovery Runbook](docs/failure-modes.md) — Fault matrix, cross-module invariants, disconnect/cancel-race case study, and recovery procedures.
- [Performance Report](docs/performance-report.md) — All 8 measured stages, separate E2E distributions, three comparative investigations, and platform caveats.
- [Showcase & Limitations](docs/showcase.md) — Terminal walkthrough, paper-arbitrage negative cases, known limitations, and verified CV summary.
