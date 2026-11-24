# Architecture & Design Notes

## System Pipeline

```text
  .mmbin / .csv replay
          │
          ▼
 ┌────────────────────┐    exchange clock (t)    ┌────────────────────────┐
 │  SimulatorEngine   │─────────────────────────►│   SimulatedExchange    │
 │ (O(1) dispatch PQ) │                          │  (Exchange IL2Book +   │
 └────────┬───────────┘                          │   QueuePriority/Touch) │
          │ t + md_latency_ns                    └───────────┬────────────┘
          ▼                                                  │ t + ack_latency_ns
 ┌────────────────────┐    ┌──────────────────┐              ▼
 │ Strategy IL2Book   │───►│  FeatureEngine   │     ┌────────────────────┐
 │ (2-level bitset)   │    │ ToB/Micro/OBI/   │     │   Portfolio &      │
 └────────────────────┘    │ OFI/RealizedVol  │     │  Markout Tracker   │
                           └────────┬─────────┘     │ (10ms / 100ms / 1s)│
                                    ▼               └────────────────────┘
                           ┌──────────────────┐
                           │    IStrategy     │
                           │ InventoryAware / │
                           │ NaiveSymmetric   │
                           └────────┬─────────┘
                                    ▼
                           ┌──────────────────┐   t + order_latency_ns
                           │   RiskManager    │───────────────────────► Exchange
                           │ (worst-case pos, │
                           │  size, notional, │
                           │  DD kill-switch) │
                           └──────────────────┘
```

## Key Engineering Decisions

### 1. Fixed-Point Prices and Dual-Clock Strict Causality
- Prices are stored as `int64_t` integer ticks (`PriceTicks`) and quantities as `int64_t` shares (`Qty`), avoiding floating-point price-level comparison hazards.
- `SimulatorEngine` maintains two completely separate L2 books (`exchange_book_` and `strategy_book_`) and two separate portfolios (`exch_portfolio` and `strat_portfolio`).
- Market events are applied immediately to `exchange_book_` at `ev.timestamp_ns` and only delivered to `strategy_book_` and `FeatureEngine` at `ev.timestamp_ns + md_latency_ns`.
- Strategy orders/cancels arrive at `SimulatedExchange` at `submit_ns + order_latency_ns`, and acks/fills arrive back at the strategy at `exch_ns + ack_latency_ns`.
- Causality is verified by `test_integration::StrictCausalityNoFutureInformationUsed`, which mutates all market events strictly after a cutoff timestamp and asserts bit-identical fills and equity points up to the cutoff.

### 2. Order Book Data Structure & Bitset Optimization Tradeoff
- Both `FlatArrayBook` and `MapBook` (`std::map`) implement `IL2Book` and are verified bit-identical across 50,000 randomized differential operations (`test_order_book`) and end-to-end replays (`test_integration`).
- An initial linear-scan `FlatArrayBook` (window = 4096 ticks) was measured at `205.7 ns` p50 (`2.46 M ev/s`) — *slower* than `std::map` (`57.3 ns` p50) because `top_levels(5)` scanned empty array slots whenever book depth was sparse.
- Adding a 2-level 64-bit hardware occupancy bitset (`std::countl_zero` / `std::countr_zero` from C++20 `<bit>`) reduced `FlatArrayBook` hot-path latency to `35.2 ns` p50 / `46.9 ns` p99.9 and increased full-replay throughput to `6.58 M ev/s` (+25% over `std::map`, 3.2x lower p99.9 tail latency).

### 3. Worst-Case Pre-Trade Risk Invariant
- `RiskManager::check_order` evaluates exposure against:
  - `worst_long = confirmed_position + open_buy_qty + new_buy_qty`
  - `worst_short = confirmed_position - open_sell_qty - new_sell_qty`
- `open_buy_qty` and `open_sell_qty` include **all** `PendingNew`, `Live`, and `PendingCancel` orders. Because an order in `PendingCancel` can still fill in a cancel race before the cancel reaches the exchange, retaining its exposure until `CancelAck{cancelled=true}` mathematically guarantees `|exchange_position| <= max_abs_position` at every nanosecond under arbitrary latency.
- When `peak_equity - current_equity >= max_drawdown_dollars`, the drawdown kill-switch fires, cancels all live/pending quotes, and rejects all subsequent orders.

### 4. Quoting Policy & Hysteresis
- `InventoryAwareStrategy`:
  - `fair_value = microprice + alpha_obi * topk_imbalance * (spread / 2) + alpha_ofi * short_horizon_ofi`
  - `reservation_price = fair_value - risk_aversion * (q_norm + quad_skew * q_norm * |q_norm|) * max(1, sigma)`
  - `half_spread = clamp(base_half_spread + vol_spread_mult * sigma + fee_buffer, min, max)`
  - Inventory-increasing quote size throttles linearly toward `min_quote_qty` and shuts off above 90% of `max_abs_position`.
- Quote-refresh hysteresis (`requote_threshold_ticks`, `min_quote_lifetime_ns`) prevents unnecessary cancel/replace churn that would forfeit queue priority.

### 5. Binary Wire Format (`.mmbin`)
- 16-byte header (`MMBIN001`, `version=1`, `flags`, `tick_size_micros`) followed by 40-byte fixed-width little-endian records, each protected by an IEEE 802.3 CRC32 checksum.
- Validated by deterministic fuzzing over 50,000 random buffers plus exhaustive 320-bit single-bit-flip testing (`test_protocol_fuzz`).
