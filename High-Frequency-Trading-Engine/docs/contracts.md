# Canonical Contracts (`contracts/v1`)

**Location:** `engine/include/hft/contracts/v1/` (`types.hpp`, `events.hpp`, `interfaces.hpp`)  
**Version:** `1.0` (`kContractVersion = 0x00010000`, wire magic `"HFT1"`)

---

## 1. Design Principles

1. **Fixed-point integer arithmetic on matching and accounting hot paths:**
   - `Price { int64_t ticks }` is expressed in instrument-specific ticks (`InstrumentMeta::tick_size_price4`).
   - `Quantity { uint64_t lots }` is expressed in instrument-specific lots (`InstrumentMeta::lot_size`).
   - `Money { int64_t scaled }` is scaled by `kMoneyScale = 10,000` ($0.0001 units, exact for ITCH `Price4` and OUCH prices).
   - Overflow-checked helpers (`price_to_price4`, `price4_to_price`, `qty_to_shares`, `shares_to_qty`, `notional_scaled`, `compute_fee`) use `__int128` intermediates and reject unaligned ticks/lots or overflow via `std::optional`.
2. **Strict clock domain separation:**
   - `SimTimeNs` (`int64_t` logical simulation nanoseconds) and `WallTimeNs` (`uint64_t` monotonic steady-clock nanoseconds) are distinct structs with **no implicit conversion** between them.
   - Deterministic replay uses `SimTimeNs` exclusively; wall-clock latency measurements in `performance` and `network-lab` modes use `WallTimeNs` exclusively.
3. **Distinct identifier domains (no ID conflation):**
   - `FeedOrderRef` (market-data feed order reference from ITCH 5.0 / MDF1) is a distinct strong type from outbound `ClientToken` (`char[14]`, OUCH-compatible) and `OrderId`.
   - `ParentOrderId` / `ChildOrderId` track Smart Order Router hierarchy.
   - `ExchangeOrderId` tracks venue-assigned resting references; `ExecId` globally deduplicates fills.
4. **Deterministic event ordering:**
   - Every scheduled event carries `EventOrderKey { event_time_ns, priority, source_sequence, insertion_sequence }`.
   - Tie-break priorities for equal `event_time_ns`:
     0. `SystemControl` (kill switch, session state, venue health)
     1. `MarketDataSnapshot`
     2. `MarketDataDelta` (exchange truth book update)
     3. `ExchangeOrderArrival` (order/cancel reaching matching engine)
     4. `ExchangeMatchAndExec` (matching fills / outbound venue report)
     5. `GatewayExecDelivery` (execution report reaching gateway / OMS / ledger)
     6. `StrategyObservation` (delayed market view delivered to strategy)
     7. `StrategyOrderIntent` (strategy order submission)
     8. `Timer`
5. **Protocol separation:**
   - **ITCH 5.0** ([`02-itch-feed-handler`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/02-itch-feed-handler)) is decoded exclusively into `MarketDataEvent` with `FeedOrderRef`; it never writes to OMS or execution state.
   - **MDF1/MDR1** ([`06-udp-multicast-feed`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/06-udp-multicast-feed)) disseminates and recovers normalized `MarketDataEvent` streams; gaps mark `FeedHealth::GapDetected` / `Resyncing` and disable strategy trading until snapshot/retransmit convergence restores `FeedHealth::Healthy`.
   - **OUCH / SoupBinTCP** ([`03-ouch-order-gateway`](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects/03-ouch-order-gateway)) carries outbound `OrderIntent` and inbound `ExecutionReport` messages, preserving token idempotence and `EventLog` reconciliation.
6. **Queue overflow contract (`QueueOverflowPolicy`):**
   - `Backpressure`: bounded spin/yield retry (never silently drops).
   - `Reject`: returns `ErrorCode::QueueFull` explicitly to caller.
   - `DropAndResync`: permitted **only** for `MarketDataEvent` payloads; strictly disallowed for `OrderIntent`, `ExecutionReport`, and `SystemEvent` (falls back to `ErrorCode::QueueFull` so order/execution events are never silently dropped).
7. **Error contract & compatibility policy:**
   - No exceptions cross hot-path adapter or transport boundaries; errors return `ErrorCode` classified into `ErrorClass::{None, Retryable, Terminal}`.
   - Wire frames validate magic `"HFT1"` and major version (`ver >> 16 == kContractVersionMajor`). Minor additions must remain backward-compatible; breaking field layout changes require bumping `kContractVersionMajor` and introducing `contracts/v2`.
