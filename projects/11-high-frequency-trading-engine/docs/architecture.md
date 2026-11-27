# Integrated HFT Platform — Architecture & Design Decisions

This document describes the integrated `High-Frequency-Trading-Engine` architecture (`engine/`) composing the ten independently runnable C++20 modules from [vaibhavkapur/High-Frequency-Trading](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects).

**Product boundary:** Educational and systems-research trading simulator. Live exchange connectivity, production certification, and live capital deployment are explicitly out of scope.

---

## 1. End-to-End Reference Architecture

```mermaid
flowchart TD
    Feed["Historical ITCH 5.0 / Synthetic L3 Feed\n(projects/02, projects/04)"]
    Decode["Feed Adapter (ItchFeedAdapter / BacktesterFeedAdapter)\nContracts/v1 MarketDataEvent + FeedOrderRef"]
    Truth["Authoritative Venue Truth Book\n(projects/01 me::Engine<PoolBook|MapBook>)"]
    MdfPub["MDF1 Multicast Publisher + FaultInjector\n(projects/06 mdf::MulticastFeedPublisher)"]
    MdfSub["MDF1 ChannelReceiver + Retransmit/Snapshot Recovery\n(projects/06 mdf::ChannelReceiver)"]
    Transport["Ordered Event Transport\nDirect In-Process OR Bounded SPSC (projects/05 lfm::SpscQueue)"]
    Lat["Causal Latency Model\n(projects/04 hftbt::LatencyPath)"]
    ObsBook["Observed Canonical Book (Per Venue/Instrument)\nFeedHealth + Staleness Gate (never sees future truth)"]
    Strat["Strategy Plugins\nSimpleDeterministic / MarketMakerPlugin (projects/08) / PaperArb (projects/10)"]
    Risk["Pre-Trade Risk Gate & Kill Switch\nWorst-case exposure incl. Pending + Uncertain"]
    Sor["Smart Order Router (Optional Multi-Venue)\n(projects/07 sor::RoutePlanner + OMS)"]
    Gw["OUCH-Style Gateway, OMS Reconciler & Binary EventLog\n(projects/03 ouch::wire + EventLog + OmsReconciler)"]
    Ledger["Fixed-Point Integer Accounting Ledger\nExact Money ($0.0001) conservation + ExecId dedup"]
    Perf["Perf Lab & Provenance Harness\n(projects/09 lpl::bench_harness + EnvironmentInfo)"]

    Feed --> Decode
    Decode --> Truth
    Decode --> MdfPub
    MdfPub -->|"MDF1 UDP / Recovery MDR1"| MdfSub
    MdfSub --> Transport
    Truth -->|"Delayed MD via SimTimeNs"| Lat
    Transport --> ObsBook
    Lat --> ObsBook
    ObsBook --> Strat
    Strat -->|"OrderIntent + ClientToken"| Risk
    Risk --> Sor
    Sor -->|"Child OrderIntents"| Gw
    Risk -->|"Direct single-venue"| Gw
    Gw -->|"OrderArrival (SimTimeNs / Loopback TCP)"| Truth
    Truth -->|"ExecutionReport + ExecId"| Gw
    Gw --> Ledger
    Ledger --> Perf
```

---

## 2. Order Execution & Causality Sequence

```mermaid
sequenceDiagram
    participant F as Feed Source (ITCH/L3)
    participant E as Authoritative Exchange (01 PoolBook)
    participant L as Causal Latency Path (04)
    participant B as Observed Book + Strategy (08)
    participant R as Pre-Trade Risk + SOR (07)
    participant O as OUCH Gateway / OMS / Journal (03)
    participant A as Fixed-Point Ledger

    F->>E: MarketDataEvent (t = T0, FeedOrderRef)
    E-->>E: Match passive orders / update truth book
    E->>L: Schedule delayed MD observation
    L->>B: Observed MD (t = T0 + md_lat >= T0)
    B->>R: OrderIntent (ClientToken, strategy_decision_time = T1)
    R-->>R: Check kill switch, stale/sync, band, notional, worst-case exposure
    R->>O: Approved intent (or child intents via SOR)
    O-->>O: Reserve PendingNew exposure + append OUCH binary journal
    O->>E: Order arrives at exchange (t = T1 + order_lat >= T1)
    E->>O: ExecutionReport (Accepted / PartialFill / Fill / Canceled, ExecId)
    O-->>O: Idempotent ExecId check + state conservation check
    O->>A: Apply fill (exact integer Money, zero double drift)
    O->>B: Notify strategy of confirmed execution report
```

---

## 3. Module Dependency Graph

```mermaid
flowchart LR
    P01["01-matching-engine\n(me::Engine<PoolBook|MapBook>)"] --> A01["matching_adapter"]
    P02["02-itch-feed-handler\n(itch::FeedHandler/decode)"] --> A02["itch_adapter"]
    P03["03-ouch-order-gateway\n(ouch::wire/EventLog/Gateway/ExchangeSim)"] --> A03["ouch_adapter"]
    P04["04-event-driven-backtester\n(hftbt::LatencyPath/synth)"] --> A04["backtester_adapter"]
    P05["05-lock-free-messaging\n(lfm::SpscQueue/affinity)"] --> A05["spsc_adapter"]
    P06["06-udp-multicast-feed\n(mdf::Publisher/Receiver/Recovery)"] --> A06["multicast_adapter"]
    P07["07-smart-order-router\n(sor::ConsolidatedBook/RoutePlanner/OMS)"] --> A07["router_adapter"]
    P08["08-market-making-simulator\n(mmsim::FeatureEngine/InventoryAware)"] --> A08["strategy_adapter"]
    P09["09-latency-performance-lab\n(lpl::bench_harness/environment)"] --> A09["perf_adapter"]
    P10["10-cross-exchange-arbitrage\n(xarb::FlatOrderBook/PaperExecutor)"] --> A10["arb_adapter"]

    A01 --> RT["hft_runtime\n(Scheduler, Book, Risk, OMS, Ledger, Pipeline)"]
    A02 --> RT
    A03 --> RT
    A04 --> RT
    A05 --> RT
    A06 --> RT
    A07 --> RT
    A08 --> RT
    A09 --> RT
    A10 --> RT

    RT --> DEMO["hft_demo CLI"]
    RT --> BENCH["hft_bench CLI"]
    RT --> TESTS["CTest Suite (6 binaries)"]
```

---

## 4. Runtime Modes, Clock Domains & Ownership

| Mode | Clock Type | Transport | Purpose |
|---|---|---|---|
| `deterministic` | `SimTimeNs` (`int64_t` logical ns) | Direct in-process scheduler | Replay determinism, causality, golden checksums, fault matrix, accounting checks |
| `network-lab` | Explicit wall-clock (`WallTimeNs`) + protocol sequence numbers | Loopback TCP (`ouch::Gateway` <-> `ExchangeSim`) + UDP Multicast (`mdf`) + 2-thread SPSC (`lfm`) | Packet loss/corruption/reordering recovery, TCP disconnect/reconnect, queue overflow behavior |
| `performance` | `WallTimeNs` (`steady_clock` monotonic ns) | Configured direct / SPSC / wire paths | Stage-level and E2E tail latency (`p50/p95/p99/p99.9/max`), throughput, comparative A/B |

### Ownership Invariants
- **Venue truth:** Owned exclusively by `MatchingEngineAdapter` (one per simulated venue).
- **Observed market view:** Owned by `CanonicalOrderBook` (one per `(VenueId, InstrumentId)`), fed strictly via delayed `StrategyObservation` events. Strategies receive `const IObservedMarketView&` and can never query `MatchingEngineAdapter` directly.
- **Order lifecycle:** Owned by `OmsReconciler` (mirrored to `OuchJournalBridge` when journaling is enabled).
- **Monetary balances:** Owned by `AccountingLedger` using exact integer `Money` ($0.0001 scaled units) and exact `cost_basis_scaled` tracking (`discrepancy_scaled == 0` verified across all fills).
- **SPSC queue ownership:** Strictly one producer thread calling `push()`, one consumer thread calling `try_pop()`, aligned to cache-line boundaries (`lfm::kCacheLine`: 128 B on Apple Silicon, 64 B on x86-64).

---

## 5. Architecture Decision Records (Summary)

- **ADR-001 (Authoritative Matching):** Project 01 `me::Engine<me::PoolBook>` is the single simulated matching engine (also plugged into Project 03 `ouch::sim::ExchangeSim` via `OuchMatchingBridge`). `me::MapBook` is retained as a differential oracle and A/B benchmark variant.
- **ADR-002 (Scheduler & Clock Separation):** `DeterministicScheduler` orders events strictly by `(event_time_ns, priority_class, source_sequence, insertion_sequence)`. `SimTimeNs` and `WallTimeNs` are distinct non-convertible C++ structs enforced via `static_assert`.
- **ADR-003 (Exact Integer Accounting & Worst-Case Risk):** `AccountingLedger` stores `cost_basis_scaled` in exact `$0.0001` integer units so `total_cash + mark_inventory == initial_cash + realized + unrealized - fees` holds with zero discrepancy. `OmsReconciler` keeps `PendingNew`, `PendingCancel`, and `Uncertain` orders in worst-case risk exposure until terminal confirmation.
- **ADR-004 (Protocol & ID Isolation):** ITCH 5.0 (`FeedOrderRef`), MDF1/MDR1 (`mdf::MarketEvent`), and OUCH/SoupBinTCP (`ClientToken`) remain explicitly separated across distinct adapters.
- **ADR-005 (Isolated Monorepo Composition):** `engine/cmake/UpstreamModules.cmake` compiles upstream sources into isolated `hft_upstream_*` static libraries without modifying `projects/` or invoking FetchContent.
