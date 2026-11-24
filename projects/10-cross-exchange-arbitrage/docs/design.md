# System Design — Cross-Exchange Arbitrage Detection & Paper Execution Engine

## 1. High-Level Architecture

```text
  Binance WS/REST ----\
  Coinbase WS --------+--> [SPSC Ring Buffers] --> Venue Adapters (isolated recovery FSMs)
  OKX WS -------------/                                      |
                                                             v
                                              Normalized L2 Books (FlatOrderBook)
                                               fixed-point ticks/lots + timestamps
                                                             |
                                                             v
                                        Depth-Aware Opportunity Scanner (6 directed routes)
                                       stale-quote guard + taker fees + pre-funded inventory
                                                             |
                                                             v
                                      Paper Order/Fill Simulator (delayed IOC legs vs truth book)
                                       pre-funded venue balances + non-instant rebalance model
                                                             |
                                                             v
                                     Audit & Analytics (JSON, CSV, SQLite3, FNV-1a run digest)
```

## 2. Core Components

### 2.1 Fixed-Point Normalization & Instrument Mapping
- Prices and quantities are parsed directly from venue JSON decimal strings into `int64_t` integer ticks (`PriceTicks`) and lots (`QtyLots`) via `parse_decimal_scaled` (`include/xarb/types.hpp`), avoiding IEEE-754 decimal drift.
- `InstrumentRegistry` (`include/xarb/instrument.hpp`) maps venue-native symbols (`BTCUSDT` on Binance, `BTC-USDT` on Coinbase and OKX) to a canonical `BTC/USDT` instrument with venue-specific tick/lot grids, taker/maker fee schedules, and on-chain withdrawal fee/latency parameters.
- Every adapter tracks both exchange timestamps (`last_exchange_ts_ns`, parsed from Binance epoch-ms, Coinbase ISO-8601 UTC, or OKX ms strings) and local receive timestamps (`last_recv_ts_ns`).

### 2.2 Isolated Venue Adapters & Recovery State Machines
Each venue lives in its own adapter subclass (`include/xarb/venue_adapter.hpp`, `src/venue_adapter.cpp`) with explicit `BookState` (`Disconnected`, `Connecting`, `Buffering`, `Synced`, `Stale`, `Resyncing`):

| Venue | Snapshot Mechanism | Delta Continuity Rule | Integrity Check | Gap/Fault Action |
|---|---|---|---|---|
| **Binance** | REST `/api/v3/depth` (`lastUpdateId`) | Buffer WS `depthUpdate`; drop `u <= lastUpdateId`; first delta bridges `U <= lastUpdateId + 1 <= u`; subsequent `U == prev_u + 1` | Sequence bridge & contiguous `U/u` | Clear book, mark `Resyncing`, buffer new deltas, request REST snapshot |
| **Coinbase** | WS `snapshot` (`sequence`) | Strict `sequence == prev_seq + 1` on `l2update` and `heartbeat`; drop `sequence <= prev_seq` | Strict monotonic sequence + heartbeat liveness | Clear book, mark `Resyncing`, request WS reconnect + fresh snapshot |
| **OKX** | WS `books` `snapshot` (`seqId`) | `prevSeqId == seqId` (no-op heartbeat), else `prevSeqId == last_seq`; drop `seqId <= last_seq` | **IEEE 802.3 CRC32** over top-25 interleaved `bid_p:bid_q:ask_p:ask_q:...` raw strings | Clear book + raw level maps, mark `Resyncing`, request WS reconnect + fresh snapshot |

Reconnect backoff uses exponential backoff (`initial_backoff_ns * multiplier^k`, capped at `max_backoff_ns`), resetting on `Synced` recovery.

### 2.3 Normalized L2 Order Books (`FlatOrderBook` vs `MapOrderBook`)
- Production hot path uses `FlatOrderBook` (`include/xarb/order_book.hpp`): contiguous sorted `std::vector<PriceLevel>` (bids descending, asks ascending), bounded by `max_depth` (default 50 levels) so steady-state updates require zero heap node allocations and sequential VWAP walks stay L1-cache resident.
- Reference `MapOrderBook` (`std::map`) is kept for differential fuzz testing (`tests/test_order_book.cpp`) and data-structure benchmarking (`benchmarks/bench_engine.cpp`).

### 2.4 Depth-Aware Marginal Executable Sizing & Opportunity Scanner
Raw top-of-book spreads overstate executable profit because deeper levels carry worse prices and taker fees apply on both legs. `compute_executable_cross` walks the buy venue's asks and sell venue's bids in tandem, accumulating size only while the marginal level pair satisfies:

$$P_{\text{sell,bid}} \cdot (1 - f_{\text{sell}}) \ge P_{\text{buy,ask}} \cdot (1 + f_{\text{buy}}) \cdot \left(1 + \frac{\text{edge}_{\min} + \text{slip\_buf}}{10{,}000}\right)$$

The resulting quantity is then capped by:
1. Per-trade risk notional cap (`max_trade_notional_quote`),
2. Pre-funded quote balance available at the buy venue (including taker fee),
3. Pre-funded base balance available at the sell venue, and
4. Common venue lot step and minimum order size.

Every gross top-of-book cross is recorded in the audit trail with its exact disposition (`actionable`, `stale_quote`, `venue_not_synced`, `crossed_book`, `insufficient_edge_after_fees`, `insufficient_depth`, `insufficient_funding`, `below_min_profit`, or `cooldown_active`).

### 2.5 Causality-Preserving Replay & Paper Execution Simulator
`ReplayEngine` (`include/xarb/replay_engine.hpp`) maintains two parallel sets of books per venue:
- **Truth books**: updated at exact recorded `recv_ns`.
- **Observed books**: updated at `recv_ns + feed_delay_ns[venue]` (and subject to injected outages).

When the scanner observes an actionable signal at `t_detect`:
1. `PaperExecutor` atomically reserves quote at the buy venue and base at the sell venue.
2. Two IOC paper legs are scheduled in the event priority queue at `t_detect + order_latency_ns[venue] ± jitter`.
3. When each leg arrives at `t_arrival`, it walks the **truth book at `t_arrival`**:
   - In **Limit-IOC** mode (`--limit-ioc 1`), the leg enforces the signal's worst-level limit price; if quotes moved away during transit, one or both legs miss, exposing `one_legged_unhedged` or `both_missed_expired` outcomes.
   - In **Market-IOC** mode (`--limit-ioc 0`), the leg walks deeper levels unconditionally, exposing `both_filled_loss_slippage` outcomes.
4. Optional non-instantaneous rebalancing (`--rebalance 1`) models withdrawal fees and `transfer_delay_ns` lock-up rather than assuming instant free transfers.
5. `PaperExecutor::verify_invariants()` asserts zero negative balances and exact base/quote conservation across fills, fees, and in-flight transfers.

### 2.6 Concurrency & Recorded Feed Formats
- `SpscQueue<T>` (`include/xarb/spsc_queue.hpp`) provides a lock-free cache-line-padded acquire/release ring buffer for decoupling per-venue receiver threads from the single-threaded book/scanner consumer loop (verified under ThreadSanitizer).
- Recorded feeds support both human-readable JSONL and a compact CRC32-validated little-endian binary format (`XARB` magic, 24-byte header, payload, 4-byte IEEE CRC32 footer), auto-detected by `read_feed_auto`.
