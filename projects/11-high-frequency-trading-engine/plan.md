# High-Frequency Trading Engine — Integration Plan

**Repository:** https://github.com/vaibhavkapur/High-Frequency-Trading  
**Proposed integration project:** `high-frequency-trading-engine`  
**Document:** `plan.md`  
**Status:** Proposed roadmap; integration work and cross-module measurements must be verified before claiming completion.  
**Primary focus:** C++20, deterministic exchange simulation, low-latency market-data and order-entry systems, market microstructure, and reproducible performance engineering.

## 1. Goal

Turn the ten **existing, independently runnable projects** in [`projects/`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects) into a coherent, end-to-end **simulated** HFT platform that:

1. Replays or generates market events; reconstructs and distributes order books.
2. Delivers causally ordered, possibly delayed market information to one or more strategies.
3. Applies trading, position, notional, quote-staleness, and kill-switch risk controls.
4. Routes orders to one or more **simulated** venues through an order-entry gateway.
5. Matches, acknowledges, fills, cancels, and reconciles orders; maintains consistent positions and P&L.
6. Supports repeatable fault injection, replay, profiling, and performance regression testing.

**Product boundary:** An educational/research system, *not* a production exchange connection, investment service, or demonstrated profitable trading strategy. Live order placement and production exchange certification are explicitly out of scope.

## 2. Existing repository inventory (reviewed 2026-11-24)

The repository is a monorepo containing **ten directories under `projects/`** (not ten separate Git repositories). Their READMEs describe implemented components, tests, benchmarks, and, in several cases, saved measured artifacts. These are **module-level claims**; an integrated platform has not yet been verified.

| Module | Existing path | Integration role | First integration action |
|---|---|---|---|
| 01 Matching Engine | [`projects/01-matching-engine`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/01-matching-engine) | Price-time matching, authoritative simulated book and fills | Wrap behind `IExchange` adapter; compare fill semantics |
| 02 ITCH Feed Handler | [`projects/02-itch-feed-handler`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/02-itch-feed-handler) | ITCH 5.0 decode/replay and reconstructed market data | Adapt decoded events; **do not** assume ITCH order IDs are outbound order tokens |
| 03 OUCH Order Gateway | [`projects/03-ouch-order-gateway`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/03-ouch-order-gateway) | OUCH-style TCP session, OMS, risk, journaling, faultable exchange | Bridge order/fill events to shared contracts; preserve reconnect semantics |
| 04 Event-Driven Backtester | [`projects/04-event-driven-backtester`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/04-event-driven-backtester) | Deterministic scheduler, delayed observations, fill/P&L research | Reuse event-time semantics; avoid dual competing sources of truth |
| 05 Lock-Free Messaging | [`projects/05-lock-free-messaging`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/05-lock-free-messaging) | Bounded SPSC inter-thread event transport | Add typed envelope and documented overflow policy |
| 06 UDP Multicast Feed | [`projects/06-udp-multicast-feed`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/06-udp-multicast-feed) | Multicast publication, gaps and retransmit/snapshot recovery | Convert normalized book events to/from its **custom MDF1** protocol |
| 07 Smart Order Router | [`projects/07-smart-order-router`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/07-smart-order-router) | Multi-venue quote aggregation, routing and parent/child OMS | Feed standardized venue quotes; map child orders to gateway tokens |
| 08 Market-Making Simulator | [`projects/08-market-making-simulator`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/08-market-making-simulator) | Inventory-aware quoting and signals | Run as a **strategy plugin**; preserve observed-vs-truth separation |
| 09 Latency Performance Lab | [`projects/09-latency-performance-lab`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/09-latency-performance-lab) | Benchmark harness, CPU/allocator analysis, performance reports | Extend with end-to-end trace and per-stage benchmarks |
| 10 Cross-Exchange Arbitrage | [`projects/10-cross-exchange-arbitrage`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/10-cross-exchange-arbitrage) | Multi-venue feed normalization and paper arbitrage | Add **paper-only** strategy adapter in final phase |

**Important distinction:** Some existing modules implement independent books, matching cores, OMSs, and replay clocks. Do not blindly link all ten and claim they form one engine. Establish shared contracts, then select one authoritative implementation per running mode.

## 3. Scope and success criteria

### MVP (`v0.1`): fully simulated, deterministic end-to-end path

- One instrument, one strategy, one simulated venue.
- Generated or file-replayed events -> normalized market state -> delayed strategy observation -> order submission -> risk -> gateway -> simulated matching -> fills -> accounting.
- New, cancel, partial fill, reject, duplicate response and disconnect/reconnect scenarios.
- An end-to-end demo command, a deterministic event/result checksum, and a regression test suite.
- Measured **end-to-end and stage-level** latency with machine/build/workload provenance.

### Integrated (`v0.2`)

- Multiple instruments and simulated venues; smart routing and market-maker strategy.
- UDP multicast dissemination, packet-loss recovery, and SPSC-based threaded pipeline.
- Synthetic-load runs, fault injection, full audit trail, cash/position/P&L reconciliation.

### Portfolio release (`v1.0`)

- ITCH historical replay or licensed sample data; paper arbitrage; comparative experiments.
- Published architecture and design decisions; reproducible benchmarks and failure-analysis notes.
- Cross-platform functional CI and separately labeled **Linux** performance experiments.
- Clear known limitations and evidence-based CV summary. **No fabricated latency or profit claims.**

## 4. Reference architecture

```text
                       +----------------------------------+
                       | Historical ITCH / Synthetic Feed|
                       +----------------+-----------------+
                                        |
                                        v
                              Feed decoder / replay
                                        |
                                 normalized MD events
                                        |
                        +---------------+----------------+
                        |                                |
                        v                                v
                 Canonical book                  UDP multicast publisher
                        |                                |
                        |                       multicast + recovery
                        |                                |
                        +-------------+------------------+
                                      |
                          ordered event transport
                          (direct / bounded SPSC)
                                      |
                            latency-aware observation
                                      |
                                      v
                        Strategy (market maker / arb)
                                      |
                                      v
                             Strategy order intent
                                      |
                                      v
                       Pre-trade risk / kill switch
                                      |
                                      v
                         Smart router (optional)
                                      |
                                      v
                         OUCH-style gateway / OMS
                                      |
                          simulated TCP session
                                      |
                                      v
                         Exchange / matching engine
                                      |
                            execution reports
                                      |
                                      v
                      OMS + positions + cash + P&L
                                      |
                          journal / traces / metrics
                                      |
                                      v
                           correctness + perf lab
```

### Runtime modes

| Mode | Timing | Transport | Use |
|---|---|---|---|
| `deterministic` | Single simulated nanosecond clock; no wall-clock sleeps | Direct in-process adapters | Core correctness, replay, causality, backtesting |
| `network-lab` | Wall-clock networking, explicit clock domains | Loopback TCP + multicast UDP | Session recovery, packet loss, operational behavior |
| `performance` | Monotonic/steady timing, pinned/isolated CPU where available | Configured direct/SPSC/network paths | Throughput, tail latency, regressions |

**Never combine simulated timestamps and wall-clock timestamps without explicit conversion.** Report measurements from these modes separately.

## 5. Canonical contracts (design before coding)

Create versioned interfaces and narrow adapters. Avoid forcing existing projects to rename every internal type.

```cpp
using InstrumentId = std::uint32_t;
using VenueId = std::uint16_t;
using OrderId = std::uint64_t;
using EventSeq = std::uint64_t;
using SimTimeNs = std::int64_t;

struct Price { std::int64_t ticks; };   // instrument-specific tick size
struct Quantity { std::uint64_t lots; }; // instrument-specific lot size

enum class Side : std::uint8_t { Buy, Sell };

struct Envelope {
    EventSeq sequence;
    SimTimeNs event_time_ns;
    InstrumentId instrument;
    VenueId venue;
    // typed payload variant (market/order/execution/system)
};
```

Required contracts and invariants:

1. **Instrument metadata:** tick size, lot size, currency, symbol mapping and fixed-point conversion. No `double` in matching/accounting hot paths; validate overflow/rounding.
2. **Identifiers:** distinct market-feed order references, client tokens, exchange IDs, parent and child order IDs. Maintain explicit correlation maps.
3. **Market data:** distinguish full snapshot from incremental update; `sequence`, `channel`, `source timestamp`, `receive timestamp`, venue, gap/stale state. ITCH 5.0 and MDF1 are **different** formats; use explicit adapters.
4. **Order intent and report:** new/cancel/replace, TIF, side, limit price, requested/remaining/cumulative fill quantity, acceptance/rejection reasons, executions, fees.
5. **State ownership:** one truth book per simulated venue; one authoritative OMS state per order; one accounting ledger. Strategies may read **observed** books, never future truth.
6. **Event ordering:** deterministic ordering `(event_time_ns, priority_class, source_sequence, insertion_sequence)`; document priorities for simultaneous market events, order arrivals, fills, and notifications.
7. **Risk:** max order size/notional/position, outstanding exposure, stale quotes, duplicate client tokens, venue health, and kill switch. Check locally before dispatch; account for pending orders.
8. **Queue contract:** bounded SPSC with explicit `backpressure`, `reject`, or `drop-and-resync`; never silently drop orders or execution reports.
9. **Error contract:** no exceptions cross hot-path or C ABI boundaries; classify retryable vs terminal errors, write structured audit events.
10. **Versioning:** `contracts/v1`; schema/fixture tests and an explicit compatibility policy before changing serialized formats.

### Integration strategy

- Prefer **thin C++ adapters** around existing libraries over copied source code or a wholesale rewrite.
- Maintain each project's ability to build and test independently.
- Add `CMake` targets or exportable libraries incrementally. Existing CMake projects may have duplicate target names and conflicting dependencies; do **not** start with `add_subdirectory()` for all ten.
- Preserve each project's original protocol dialect and document the supported subset, rather than declaring the OUCH-style gateway a drop-in certified Nasdaq OUCH implementation.
- Prefer a single process for deterministic tests; enable threads and sockets only in dedicated modes.

## 6. Proposed repository additions

```text
High-Frequency-Trading/
├── README.md                         # existing
├── plan.md                           # this file (proposed at repo root)
├── projects/                         # ten existing standalone projects
├── engine/                           # NEW: composition/orchestration only
│   ├── CMakeLists.txt
│   ├── include/hft/contracts/v1/
│   ├── include/hft/runtime/
│   ├── src/runtime/
│   ├── src/adapters/
│   │   ├── matching_adapter.cpp
│   │   ├── itch_adapter.cpp
│   │   ├── ouch_adapter.cpp
│   │   ├── multicast_adapter.cpp
│   │   ├── router_adapter.cpp
│   │   └── strategy_adapter.cpp
│   ├── apps/hft_demo.cpp
│   ├── config/
│   └── tests/
│       ├── contract/
│       ├── integration/
│       ├── failure/
│       └── golden/
├── integration/                      # NEW: fixtures, scenarios and reports
│   ├── scenarios/
│   ├── fixtures/
│   └── results/
├── scripts/                          # NEW: build, demo, bench, summarize
├── docs/                             # NEW: architecture, ADRs, limitations
└── .github/workflows/                # NEW: monorepo CI
```

These paths are **proposed**, not a statement that the engine, top-level scripts, or CI currently exist.

## 7. Execution roadmap and dependencies

**Order of work:** correctness-first, then reliable integration, then performance. Schedule estimates are illustrative for one engineer and should not be treated as a delivery commitment.

| Phase | Indicative effort | Deliverable | Depends on | Exit gate |
|---|---:|---|---|---|
| 0 · Baseline & inventory | 2–3 days | Build/test inventory, CLI demos, metrics provenance | Existing 10 projects | All failing/unsupported platforms documented |
| 1 · Contract layer | 3–5 days | Shared types, adapters, canonical event model | Phase 0 | Contract fixtures and mapping tests pass |
| 2 · Core execution MVP | 5–8 days | Replay -> strategy -> risk -> gateway -> matching -> fills | Phase 1; 01/02/03/04 | One-instrument deterministic golden run passes |
| 3 · Accounting & failure recovery | 4–6 days | Event journal, OMS recovery, reconciliation | Phase 2; 03/04 | Duplicate/disconnect/race tests pass |
| 4 · Network & concurrency | 5–8 days | UDP multicast + recovery + bounded SPSC | Phase 3; 05/06 | Loss/reordering and queue-full scenarios converge |
| 5 · Multi-venue strategy platform | 5–8 days | Router and market-making plugin | Phase 4; 07/08 | Risk and execution reconciliation across venues |
| 6 · Latency & scaling | 4–7 days | End-to-end perf harness, comparisons, regressions | Phases 2–5; 09 | Repeatable raw runs; no correctness regression |
| 7 · Paper arbitrage & showcase | 3–6 days | Arbitrage strategy, examples, documentation | Phase 5; 10 | Causal paper runs; limits and caveats validated |

### Phase 0 — baseline and audit

- [ ] Record commit SHA, compiler, CMake version, OS, CPU, architecture, dependencies and build modes.
- [ ] Run existing project demos and `ctest` individually; record pass/fail/skip **without asserting results from README text alone**.
- [ ] Inventory project library exports, API shape, dependencies, naming collisions and competing duplicate implementations.
- [ ] Freeze baseline behavior with sample fixtures and output hashes.
- [ ] Open architecture decisions: matching-engine authority, scheduler authority, ledger authority, versioned adapters.

**Deliverable:** `docs/integration-audit.md`, compatibility matrix, baseline test report.

### Phase 1 — contracts and adapters

- [ ] Define instruments, ticks/lots, timestamps, event envelope, MD snapshot/delta, order intent, execution report.
- [ ] Implement 01/02/03/04 adapters in isolation with fixture-driven unit tests.
- [ ] Define deterministic sequencing, clock boundaries and event versioning.
- [ ] Ensure `ITCH -> observed market state` is not confused with `OUCH -> execution flow`.
- [ ] Validate integer conversion/overflow, byte order, duplicate IDs, stale data and rejection paths.

**Deliverable:** `engine/include/hft/contracts/v1`, adapter tests, `docs/contracts.md`.

### Phase 2 — first end-to-end exchange loop

- [ ] Add `hft_demo --mode deterministic --scenario single-venue`.
- [ ] Replay a tiny deterministic L3 fixture; feed a delayed market view to a strategy.
- [ ] Create a simple strategy placing and cancelling limit orders with explicit risk checks.
- [ ] Route its intents through the gateway adapter to the authoritative simulated matching engine.
- [ ] Handle accepted, partially executed, fully executed, cancelled and rejected outcomes.
- [ ] Produce normalized event trace, order lifecycle, final book, cash and position summary.
- [ ] Make replay output stable independent of machine scheduling and hash the result.

**Deliverable:** one runnable complete-system demo and a golden expected-output fixture.

### Phase 3 — correctness, recovery and risk hardening

- [ ] Inject duplicates, delayed acknowledgments, lost responses, reconnects and cancellation/fill races.
- [ ] Reuse the gateway journal/reconciliation model; do not blindly resend uncertain orders under new tokens.
- [ ] Reconcile accepted quantity = filled + remaining + cancelled as appropriate for each terminal state.
- [ ] Ensure no unacknowledged order disappears from worst-case risk exposure.
- [ ] Reconcile execution ledger against reported positions/cash/P&L; fail the test on discrepancies.
- [ ] Add risk tests for stale markets, invalid prices, oversize requests, venue outages and kill switch.

**Deliverable:** fault matrix and recovery runbook in `docs/failure-modes.md`.

### Phase 4 — multicast and low-latency transport

- [ ] Publish normalized events through module 06's **custom** multicast protocol.
- [ ] Exercise sequence gaps, snapshots, retransmit, corruption, duplication and out-of-order recovery.
- [ ] Insert module 05's SPSC queues at explicit thread boundaries.
- [ ] Document queue ownership, memory-ordering assumptions, overload behavior and backpressure.
- [ ] Verify subscribers converge on the same state after recovery; prevent trading on out-of-sync books.
- [ ] Keep a direct in-process mode as the deterministic baseline.

**Deliverable:** `network-lab` demo with recoverable injected losses.

### Phase 5 — multi-venue and strategies

- [ ] Add multiple venue books, fee schedules, independent latencies, health status and balances.
- [ ] Integrate router 07: compare quoted vs executable price, fee, depth, staleness and latency.
- [ ] Integrate strategy 08: signal generation, reservation price, spread/inventory skew and lifecycle.
- [ ] Track parent/child orders, venue-specific fills, inventory and risk across simulated venues.
- [ ] Add walk-forward and out-of-sample experiments; mark all synthetic experiments as synthetic.

**Deliverable:** multi-venue smart-routing and inventory-aware quoting demo.

### Phase 6 — reproducible performance engineering

- [ ] Instrument decoding, book update, strategy compute, risk, routing, serialization, matching and OMS update stages.
- [ ] Record end-to-end **tick-to-decision**, **order-submit-to-ack**, and **submit-to-fill** distributions (separately).
- [ ] Measure p50/p95/p99/p99.9/max, throughput and drops under stated load; include warmup and sample sizes.
- [ ] Compare direct vs SPSC vs UDP/TCP modes without mixing timing domains.
- [ ] On Linux, optionally use `perf stat`, CPU affinity, page faults and cache-miss counters; note privileges and unsupported platforms.
- [ ] Store raw JSON/CSV, configuration, compiler flags, CPU metadata, Git SHA and report-generation scripts.
- [ ] Compare to baseline using confidence/repetition; investigate improvements that degrade tail latency.

**Performance goals are hypotheses, not promises:** start with zero correctness violations and no unexplained loss. Set numerical p99/throughput targets only **after** Phase 0 baselines on fixed hardware and workloads.

**Deliverable:** `docs/performance-report.md`, scripted repeatable benchmark output.

### Phase 7 — arbitrage and public portfolio release

- [ ] Run module 10 as a paper-only strategy on normalized multi-venue snapshots.
- [ ] Include fees, depth, inventory funding, latency, staleness and partial/one-legged execution risks.
- [ ] Demonstrate negative cases where a gross spread fails net-execution feasibility.
- [ ] Add one-command scripted demo and architecture sequence diagram.
- [ ] Publish reproducibility guide, known limitations, benchmark provenance and results index.
- [ ] Create CV bullets only from **verified integration measurements**; distinguish component benchmarks from system numbers.

**Deliverable:** `v1.0` portfolio demonstration, not a production deployment.

## 8. Testing and acceptance matrix

| Level | Required cases | Evidence |
|---|---|---|
| Contract | Price/lot mapping, bad inputs, endian/round-trip, version mismatches | Unit tests + golden fixtures |
| Matching | Price-time priority, self-consistency, cancels, partial fills, conservation | Differential / property tests |
| Market data | Snapshots/deltas, gaps, duplicate/reordered updates, stale flags | State hash vs reference book |
| Order/OMS | Rejects, partial fills, cancel/replace races, token idempotence | State transitions + journal replay |
| Strategy causality | No action on unobserved data; no fill before order arrival | Deterministic event trace |
| Risk/accounting | Outstanding exposure, position/cash conservation, kill switch | Ledger cross-check and negative tests |
| Networking | Packet loss/corruption, reconnect, snapshot recovery | Convergent state after fault recovery |
| Concurrency | Queue full, stop/shutdown, ownership/memory safety | Stress + TSAN where supported |
| System | One-venue and multi-venue golden scenarios | Full-run checksum and invariant report |
| Performance | Load scaling, per-stage and E2E tail distributions | Raw data + environment + reproducible script |

### Cross-module invariants

- No future information reaches the strategy; observation time >= source event time.
- An order can fill only after the exchange has received it.
- One execution ID is counted at most once in fills, cash and position.
- `cum_filled <= submitted_qty`, and order state/remaining quantity always agree.
- Monetary P&L reconciles with cash, marked inventory and fees according to one documented accounting model.
- Sequence gaps mark a market view unusable until recovery; recovery restores the same book hash as the reference.
- No message loss may be silently interpreted as a successful order cancellation.
- Replay with the same seed, config and fixture yields identical normalized event hash.

## 9. Build, developer workflow and CI

Existing modules are independently buildable. Start with their documented commands, e.g.:

```bash
cmake -S projects/01-matching-engine -B build/matching -DCMAKE_BUILD_TYPE=Release
cmake --build build/matching --parallel
ctest --test-dir build/matching --output-on-failure
```

**Proposed** top-level workflow once an engine CMake project exists:

```bash
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine --parallel
ctest --test-dir build/engine --output-on-failure
./build/engine/hft_demo --mode deterministic --scenario single-venue
```

Do not advertise the proposed `hft_demo` executable as implemented until it builds and runs.

Recommended CI jobs: independent per-project C++ builds, engine contract/integration tests, Clang/GCC Linux smoke builds, clang-format, warning-as-error build, ASan+UBSan, TSan on a supported runner, deterministic fixture replay, and performance collection **outside** shared-runner pass/fail gates. Keep benchmark data as artifacts; noisy shared CI hosts are not reliable absolute-latency references.

## 10. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Ten modules use different matching/order-book semantics | Incorrect fills / double state | Choose one venue-truth implementation; differential validation |
| Tick/lot/currency scales differ | Wrong orders and P&L | Canonical metadata and checked conversions |
| Simulation time mixed with wall-clock | Lookahead and invalid latency claims | Separate runtime modes and clock types |
| ITCH, OUCH-style and MDF1 conflated | Broken wire assumptions | Explicit versioned, tested protocol adapters |
| Duplicate fills / replay after reconnect | Incorrect risk and cash | Stable IDs, idempotence, journal reconstruction |
| SPSC queue saturation | Lost order/control messages | Bounded policies, resync, counters and alarms |
| Hidden CMake target/dependency conflicts | Fragile monorepo build | Exported libraries, build isolation, incremental linking |
| Synthetic profits mistaken for real edge | Misleading portfolio | Reproducible assumptions, out-of-sample and negative cases |
| Microbenchmark wins don't improve E2E latency | Misleading performance work | Measure both, report p99 and confidence across repetitions |

## 11. First five pull requests (small, reviewable increments)

1. **PR 1 — Integration audit:** `docs/integration-audit.md`, build matrix, selected authoritative components and open ADRs. **Pass:** independent smoke tests logged.
2. **PR 2 — Contracts:** `engine/include/hft/contracts/v1/` + tick/lot/id/clock mapping tests. **Pass:** contract fixtures and overflow tests.
3. **PR 3 — Replay-to-book:** ITCH/synthetic input adapter, canonical book, one golden event stream. **Pass:** deterministic book hash.
4. **PR 4 — Gateway execution loop:** risk -> order gateway -> simulated exchange -> fill/OMS/ledger. **Pass:** deterministic end-to-end order lifecycle.
5. **PR 5 — Failure scenarios and demo:** journal recovery, duplicate and cancel races, CLI demo, documentation. **Pass:** seeded fault matrix and unchanged golden baseline.

Then implement multicast/SPSC, multi-venue strategies, and benchmark infrastructure in subsequent PRs.

## 12. Definition of done

- [ ] Fully working `single-venue` deterministic run with stable replay checksum.
- [ ] Architecture shows exact producers/consumers, state ownership, timing domains and protocol boundaries.
- [ ] No unexplained test failures in documented supported environments.
- [ ] Fault cases cover lost/duplicated/out-of-order data and gateway reconnect uncertainty.
- [ ] Position, cash, fees and exposure reconcile to execution history.
- [ ] Multi-venue demo supports at least one smart router and one market-making strategy.
- [ ] Raw, reproducible benchmarks include throughput and p50/p99/p99.9 with full provenance.
- [ ] Standalone module builds and tests remain functional.
- [ ] One-command demonstration and clear limitations are published.
- [ ] CV claims are precise, verifiable, and separate standalone-module results from integrated-system results.

## 13. Public showcase deliverables

- 2–3 minute terminal/video walkthrough: feed -> strategy -> order -> fill -> risk/P&L.
- Architecture diagram, event sequence, module dependency graph and ADRs.
- Two comparative investigations: e.g. map vs pool matching, direct vs queued feed, recoverable vs stale multicast.
- One fault-injection case study showing an actual invariant enforced.
- One documented end-to-end benchmark result on fixed hardware.
- Short CV project description backed by links to code, tests, benchmark scripts and measured results.

**Suggested eventual CV wording (only after the integrated run is verified):**

> Built a C++20 simulated HFT platform integrating exchange-style matching, ITCH market-data replay, OUCH-style order management, bounded inter-thread messaging, UDP multicast recovery, risk controls, and multi-venue execution; validated deterministic replay, recovery invariants, and reproducible end-to-end tail-latency benchmarks.
