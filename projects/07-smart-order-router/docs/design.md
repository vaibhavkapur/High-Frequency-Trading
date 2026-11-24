# Smart Order Router — Design & Architecture

## Overview

`sor` is a single-threaded, zero-allocation-on-hot-path C++20 multi-venue Smart Order Router and Parent/Child Order Management System with an integrated discrete-event multi-venue execution simulator.

```text
  Venue A wire/L2 feed --\
  Venue B wire/L2 feed ---> QuoteNormalizer ---> ConsolidatedBook ---> RoutePlanner
  Venue C wire/L2 feed --/     (seq/ts/scale)     (per-venue L2,         |   ^
                                                   staleness, BBO)       |   |
                                                                         v   |
                                                  RiskManager <----> OrderManagementSystem
                                                (limits, dedup,       (parent/child FSM,
                                                 price collar)         cancel/replace,
                                                                       failover, audit)
                                                                         |   ^
                                                                         v   |
                                                                   VenueHealthMonitor
                                                                  (EWMA RTT/fill-prob,
                                                                   circuit breaker)
```

## Key Data Structures & Hot-Path Design

### 1. Fixed-Point Pricing (`include/sor/types.hpp`)
- Prices are represented as integer `PriceTicks` (`int64_t`) scaled by `kPriceScale = 10,000` ($0.0001 / 1 mil per tick).
- Sub-penny maker/taker fees (e.g., `+$0.0030/share` = `30` ticks; `-$0.0018/share` rebate = `-18` ticks) and bps fees (`taker_fee_bps`) are represented without binary floating-point tick drift.

### 2. Binary Wire Quote Normalization (`include/sor/quote_normalizer.hpp`)
- Compact little-endian wire format (`"SORQ"` magic, version 1, 40-byte header, up to `kMaxBookDepth = 16` 20-byte levels, 4-byte FNV-1a checksum).
- Decoded safely via `std::memcpy` with strict bounds, enum, timestamp, price/quantity, and checksum checks.
- `QuoteNormalizer` tracks per-venue sequence numbers (dropping duplicates/out-of-order updates and counting gaps), clamps negative feed latency due to clock skew, and scales raw venue ticks to canonical ticks.

### 3. Consolidated L2 Order Book (`include/sor/consolidated_book.hpp`)
- Fixed-capacity sorted arrays (`kMaxVenues = 8`, `kMaxBookDepth = 16`) per venue (`VenueBook`) with zero dynamic allocations during updates or reads.
- Tracks `last_exchange_ts_ns` and `last_recv_ts_ns` per venue; venues whose quote age `now_ns - last_recv_ts_ns > stale_timeout_ns` are marked ineligible (`STALE_QUOTE`).
- Computes simulated consolidated raw BBO and explicit fee-adjusted effective BBO (`raw_ask + taker_fee` for buys; `raw_bid - taker_fee` for sells).

### 4. Explainable Cost Objective & Route Planning (`include/sor/route_planner.hpp`)
Every candidate venue level `(venue, level)` is scored using an explicit, auditable 4-term cost breakdown (all in bps relative to arrival consolidated mid $M$):

$$\text{TotalExpectedCost}_{\text{bps}} = C_{\text{raw}} + C_{\text{fee}} + C_{\text{latency}} + C_{\text{miss}}$$

1. **Raw price cost**:
   $$C_{\text{raw}} = \frac{\text{side\_sign} \cdot (P_{\text{raw}} - M)}{M} \times 10^4$$
2. **Explicit venue fee (negative for taker rebates)**:
   $$C_{\text{fee}} = \frac{\text{fee}_{\text{per\_share}} + P_{\text{raw}} \cdot \text{fee}_{\text{bps}}/10^4}{M} \times 10^4$$
3. **Latency & quote-age adverse selection risk**:
   $$t_{\text{eff}} = \hat{\ell}_{\text{one-way}} + w_{\text{age}} \cdot \text{age}_{\text{quote}}, \quad C_{\text{latency}} = \alpha_{\text{adv}} \cdot t_{\text{eff, ms}}$$
4. **Fill-probability miss / re-routing penalty**:
   $$\hat{p}_{\text{fill}} = \text{clip}\!\left(p_0 \cdot \gamma_{\text{health}} \cdot e^{-\lambda t_{\text{eff, ms}}} \cdot (1 - \beta \cdot \tfrac{q_{\text{take}}}{q_{\text{disp}}}),\; 0.05,\; 0.999\right)$$
   $$C_{\text{miss}} = (1 - \hat{p}_{\text{fill}}) \cdot \kappa_{\text{miss}}$$

Four routing policies share the same candidate pipeline for controlled comparisons:
- `naive_primary`: baseline routing exclusively to primary venue (`venue 0`).
- `naive_best_price`: baseline sorting purely by $C_{\text{raw}}$ (ignores fees, latency, fill probability).
- `fee_aware`: sorts by $C_{\text{raw}} + C_{\text{fee}}$ with staleness and health gating.
- `smart_cost_aware`: sorts by $\text{TotalExpectedCost}_{\text{bps}}$ with staleness and health gating.

Allocation respects `remaining_qty`, `min_order_qty`, `max_child_qty`, `lot_size`, `max_active_children`, `max_parent_share`, parent `limit_price`, and `FOK` all-or-nothing constraints — all using fixed stack buffers (`RoutePlan`) with zero heap allocations.

### 5. Parent/Child OMS, Venue Health & Pre-Trade Risk (`include/sor/oms.hpp`, `risk_manager.hpp`, `venue_health.hpp`)
- **Pre-trade risk checks** (run before any child order is created): global kill switch, duplicate `client_order_id` rejection, max order quantity & notional, price collar vs. consolidated mid, sliding 1-second order rate limit, and worst-case net/gross position & open notional exposure (accounting for in-flight working orders).
- **Exact-once partial-fill accounting**: inbound `ExecutionReport::Fill` messages are deduplicated by `exec_id`; duplicate reports are audited (`DUPLICATE_EXEC_IGNORED`) and never double-decrement `parent.leaves_qty` or double-update risk position.
- **Cancel/Replace & Automatic Failover**: supports `cancel_parent`, `cancel_child`, `replace_child`, and `on_venue_down`. Consecutive rejects/timeouts trip `VenueHealthMonitor` to `CIRCUIT_OPEN`, preventing newly routed child orders from hitting a failing venue while automatically re-routing unallocated parent quantity to remaining healthy venues (bounded by `max_reroute_attempts`).
- **Audit Log (`include/sor/audit_log.hpp`)**: every submission, risk check, route decision (including per-venue cost breakdown and ineligibility reasons), child lifecycle event, duplicate ignore, health transition, and failover is logged and exportable to JSONL.

### 6. Discrete-Event Multi-Venue Simulator (`include/sor/simulator.hpp`)
- Maintains **separate true venue books** vs. the **router's perceived consolidated book** (fed through delayed `FeedDelivery` events).
- Decouples `market_rng`, `order_rng`, and `exec_rng` so all routing policies face the **exact same exogenous market tape and parent order stream**.
- Simulates one-way network latency + jitter, in-flight queue depletion from competing aggressive flow (with adverse level shift), partial fills, IOC residual cancels, stale-feed freezes, and mid-session venue outages.
