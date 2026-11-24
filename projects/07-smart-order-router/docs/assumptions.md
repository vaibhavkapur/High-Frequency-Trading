# Assumptions, Scope & Limitations

## Regulatory & Market Scope
- **Simulated US equities-style market model**: the default three venues (`ARCA`, `BATS_INV`, `IEX_SIM`) illustrate common US equities microstructure tradeoffs (maker-taker fees, inverted taker-rebate venues, and speed-bump latency asymmetry).
- **Not a compliant production broker router**: this project does **not** implement Regulation NMS (Order Protection Rule / Rule 611), SIP vs. direct-feed latency arbitrage rules, Rule 15c3-5 certified broker risk controls, locked/crossed market exception tags (ISO), or real exchange FIX/OUCH/ITCH sessions.
- **Consolidated book != Regulatory NBBO**: `ConsolidatedBook::consolidated_bbo()` produces an educational consolidated view over configured simulated feeds; it must not be treated as a legally compliant NBBO.

## Modeling Assumptions
1. **Quote liquidity is not guaranteed**: displayed L2 quantities can be partially or fully depleted by competing market flow during child order flight time (`one_way_latency_ns`).
2. **Fill-probability model is parametric**: `RoutePlanner` uses an exponential latency/age decay and linear displayed-depth take-ratio penalty, combined with an EWMA of observed venue fill ratios. Real fill probabilities depend on hidden/reserve liquidity, queue position, order-book imbalance, and cross-venue correlation.
3. **Adverse selection model is stylized**: in the simulator, competing-flow depletion shifts remaining contra levels adversely by 1 tick (`$0.01`) to capture the cost of being picked off on slow venues. Real market impact is non-linear and stateful across venues.
4. **Explicit fees only**: venue cost calculations model per-share fixed fees/rebates and proportional bps fees. Tiered monthly volume rebates, clearing/OCC/SEC/FINRA fees, and exchange port costs are omitted.
5. **Single-threaded deterministic core**: `ConsolidatedBook`, `RoutePlanner`, `RiskManager`, and `OrderManagementSystem` are designed for a single-threaded event loop (typical of pinned HFT router threads). Concurrent cross-thread usage requires external synchronization or SPSC queues.
6. **Single-symbol book per `ConsolidatedBook` instance**: multi-symbol routing would shard one `ConsolidatedBook` + `RoutePlanner` per symbol (or symbol hash table) while sharing `RiskManager` portfolio exposure limits.
