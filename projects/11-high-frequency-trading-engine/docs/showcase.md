# Portfolio Showcase, Walkthrough & Limitations

This guide provides a reproducible 2–3 minute terminal walkthrough of the integrated C++20 HFT simulator, summarizes the paper-only arbitrage negative cases, states known limitations transparently, and provides evidence-based CV wording separated cleanly from standalone module metrics.

---

## 1. One-Command End-to-End Walkthrough

```bash
# Build with strict warnings-as-errors and run all 6 CTest suites (17 test cases)
./scripts/build_all.sh

# Run the complete portfolio showcase (deterministic golden + all scenarios + network-lab + perf)
./scripts/run_demo.sh

# Optional: include live loopback TCP (OUCH Gateway <-> PoolBook ExchangeSim) & UDP Multicast
LIVE_SOCKETS=1 ./scripts/run_demo.sh

# Run full reproducible benchmarks and regenerate CSV/JSON/Markdown reports
./scripts/run_benchmarks.sh integration/results/apple_m5_release
```

### What the Walkthrough Demonstrates

1. **Deterministic Single-Venue MVP (`--mode deterministic --scenario single-venue`):**
   - Replays a deterministic L3 fixture through Project 01 (`me::Engine<me::PoolBook>`), applies causal latencies via Project 04 (`hftbt::LatencyPath`), enforces pre-trade risk (rejecting an intentional oversize probe with `RISK_MAX_ORDER_QTY`), matches aggressive IOC partial+full fills and passive resting orders + cancels, reconciles exact integer cash/position/fees/unrealized P&L (`discrepancy_scaled == 0`), and verifies bit-identical output against [`engine/tests/golden/single_venue.golden.json`](../engine/tests/golden/single_venue.golden.json) (`replay_checksum = 66d95957646657a6`, `final_book_hash = 0e6635ac5e8eb3f6`).
2. **ITCH 5.0 Replay (`--scenario itch-replay`):**
   - Decodes 1,508 synthetic Nasdaq ITCH 5.0 frames through Project 02 (`itch::FeedHandler`) into 1,556 canonical `MarketDataEvent`s using explicit `FeedOrderRef` (separate from outbound `ClientToken`), confirms exact BBO agreement with Project 02's reference `BookBuilder`, and executes causally with zero violations.
3. **Fault Injection & Recovery (`--scenario fault-recovery`):**
   - Injects duplicate fills, delayed acks, a dropped response, and a mid-session gateway disconnect; verifies idempotent `ExecId` deduplication (`duplicates_ignored = 2`), worst-case exposure preservation during `Uncertain` states, and clean replay of Project 03's binary `ouch::EventLog`.
4. **Multicast Recovery & Bounded SPSC (`--mode network-lab --scenario all`):**
   - Exercises Project 06's custom `MDF1/MDR1` wire framing under 15% drops, 10% duplicates, 8% CRC corruption, and 15% reordering (`pub_hash == sub_hash == 416d1d4c62c1c1e8`), plus Project 05's two-thread `lfm::SpscQueue` verifying zero drops and identical book hash (`607efce1739cb560`).
5. **Multi-Venue Smart Routing & Market Making (`--scenario multi-venue-sor`, `--scenario multi-venue-mm`):**
   - Routes across 3 simulated venues (`ARCA_SIM`, `BATS_INV`, `IEX_SIM`) with distinct fee schedules (maker rebates vs inverted taker rebates), latencies, and staleness gates using Project 07's `SmartOrderRouter`, and runs Project 08's `InventoryAware` market-maker plugin across synthetic in-sample and out-of-sample walk-forward seeds.
6. **Paper-Only Cross-Exchange Arbitrage (`--scenario paper-arb`):**
   - Runs Project 10's paper arbitrage engine on normalized multi-venue books and explicitly demonstrates **negative cases** where positive gross spreads fail net execution feasibility:
     - **Fee erosion:** +15 bps gross cross rejected because combined taker fees (22 bps) make net edge negative.
     - **Stale quotes:** +80 bps gross cross rejected because sell venue quote age (80 ms) exceeds 50 ms staleness limit.
     - **One-legged latency risk:** Actionable cross at detection time where the sell venue reprices downward before the sell leg arrives, resulting in an unhedged single-leg fill (`one_legged_unhedged = 1`) rather than riskless profit.

---

## 2. Known Limitations & Product Boundary

- **Not a production trading system:** No live broker/exchange connectivity, no FIX/OUCH/ITCH production certification, and no real money or order execution.
- **Protocol subsets:** The OUCH-style gateway ([`03-ouch-order-gateway`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/03-ouch-order-gateway)), ITCH 5.0 decoder ([`02-itch-feed-handler`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/02-itch-feed-handler)), and custom `MDF1/MDR1` multicast protocol ([`06-udp-multicast-feed`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/06-udp-multicast-feed)) from [`vaibhavkapur/High-Frequency-Trading`](https://github.com/vaibhavkapur/High-Frequency-Trading) implement documented educational subsets, not drop-in exchange-certified production gateways.
- **Synthetic market data:** All included walkthroughs and walk-forward experiments use deterministic synthetic feeds. Simulated P&L on synthetic order flow is **not** evidence of live trading alpha or profitability.
- **Platform performance caveats:** Saved benchmarks in `integration/results/apple_m5_release/` were measured on macOS / Apple Silicon (Apple M5), where Linux `perf_event_open` cache-miss counters, `recvmmsg`, `SO_BUSY_POLL`, and hard CPU core pinning (`pthread_setaffinity_np`) are unavailable. Linux performance runs should be collected on dedicated non-shared Linux hardware.
- **Standalone vs integrated builds:** Projects `01`, `02`, `03`, `04`, and `07` fetch GoogleTest / Catch2 / Google Benchmark via CMake `FetchContent` when their standalone test suites are enabled; `engine/` intentionally uses a zero-external-dependency CTest harness so the integrated platform builds and tests completely offline.

---

## 3. Verified, Evidence-Based CV Summary

> **Note:** Separate standalone component microbenchmarks from integrated system metrics, and cite only verified artifacts present in this repository.

- **Integrated platform summary:** Built a C++20 simulated HFT platform (`engine/`) composing ten standalone systems modules (price-time matching engine, ITCH 5.0 decoder, OUCH-style TCP gateway/journal, causal event scheduler, wait-free SPSC queues, MDF1 UDP multicast with gap/snapshot recovery, multi-venue smart order router, inventory-aware market maker, latency benchmark lab, and paper arbitrage simulator) behind versioned fixed-point contracts (`contracts/v1`).
- **Correctness & recovery verification:** Enforced deterministic replay (`replay_checksum` bit-identical across `PoolBook` and `MapBook`), exact `$0.0001` fixed-point cash/position/P&L conservation (`discrepancy == 0`), worst-case pending/uncertain risk exposure across disconnects, idempotent `ExecId` deduplication, and ASan+UBSan/TSan-clean execution across 6 integrated CTest suites (17 test cases).
- **Measured integrated performance (Apple M5, AppleClang 21, `-O3 -DNDEBUG`, `integration/results/apple_m5_release/`):**
  - Integrated hot-path distributions (500 post-warmup ops): `tick-to-decision` p50 `333 ns` / p99 `1,125 ns`; `order-submit-to-ack` p50 `125 ns` / p99 `791 ns`; `order-submit-to-fill` p50 `167 ns` / p99 `876 ns`.
  - Steady-state 10k matching A/B: `PoolBook` `16.24 ns/op` p50 (`61.6 Mops/s`) vs `MapBook` `34.12 ns/op` (`29.3 Mops/s`, `2.10×` speedup) with identical state hashes.
  - Bounded 2-thread SPSC handoff (`lfm::SpscQueue`): `54.46 ns/op` p50 (`18.4 Mops/s`), zero drops, exact book convergence; MDF1 multicast with 15% drops + 10% reordering converges to identical publisher state hash with `+22.8%` recovery overhead (`310.75 ns/op` vs `253.12 ns/op`).
