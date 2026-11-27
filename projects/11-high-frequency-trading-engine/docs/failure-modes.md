# Failure Modes, Invariants & Recovery Runbook

This document defines the fault matrix tested in `engine/tests/failure/test_failure_recovery.cpp` and `engine/tests/integration/test_multicast_spsc.cpp`, along with the recovery runbook and a concrete fault-injection case study.

---

## 1. Cross-Module Invariants Verified Automatically

Every test suite and `hft_demo` execution checks these invariants and fails immediately on any violation:

| # | Invariant | Enforced By | Automated Verification |
|---|---|---|---|
| 1 | No future information reaches strategy (`observation_time >= source_event_time`) | `DeterministicScheduler` + `CausalLatencyAdapter` | `test_deterministic_e2e`, `DeterministicRunResult::causality_violations == 0` |
| 2 | Orders can only fill after arriving at exchange (`exchange_time >= order_sent_time`) | `DeterministicScheduler` | `test_deterministic_e2e`, `pipeline.cpp` |
| 3 | Each `(VenueId, ExecId)` is counted at most once in fills, cash, fees, and position | `OmsReconciler` + `AccountingLedger` | `test_failure_recovery::DuplicateFillsAndReconnectExposureConservation` |
| 4 | `cum_filled <= submitted_qty` and `filled + leaves + canceled == submitted_qty` | `OmsReconciler::check_invariants()` | All integration & failure tests |
| 5 | `cash + mark_inventory == initial_cash + realized + unrealized - fees` (`discrepancy == 0`) | `AccountingLedger::reconcile()` | `test_failure_recovery::AccountingLedgerExactConservationThroughPositionFlips` |
| 6 | Multicast gaps mark market view non-tradable until recovery restores identical book hash | `MulticastFeedAdapter` + `CanonicalOrderBook` + `PreTradeRiskGate` | `test_multicast_spsc::MdfLargeGapTriggersSnapshotFallbackAndBlocksTradingWhileOutOfSync` |
| 7 | Lost cancel response never clears exposure; fill/cancel race preserves `Filled` | `OmsReconciler` | `test_failure_recovery::DuplicateFillsAndReconnectExposureConservation` |
| 8 | Same seed + config + fixture produces bit-identical normalized event checksum & book hash | `DeterministicEnginePipeline` | `test_deterministic_e2e` + `engine/tests/golden/single_venue.golden.json` |

---

## 2. Fault Injection Matrix

| Fault Category | Injected Condition | Expected System Behavior | Verified In |
|---|---|---|---|
| **Duplicate execution reports** | Same `ExecutionReport` (`ExecId`) delivered twice | Second copy ignored (`duplicates_ignored++`); position, cash, and fees unchanged | `test_failure_recovery`, `hft_demo --scenario fault-recovery` |
| **Delayed acknowledgments** | Ack delayed +100–120 µs | Order remains `PendingNew` and reserved in worst-case exposure until ack/fill | `test_failure_recovery` |
| **Lost responses + gateway disconnect** | Report dropped mid-session followed by `SessionDown` | Non-terminal orders transition to `Uncertain`; **exposure is NOT released**; reconnect reconciles via sequenced truth without minting new tokens | `test_failure_recovery`, `hft_demo --mode network-lab --scenario ouch-tcp` |
| **Cancel / fill race** | Resting order fills while `Cancel` is in flight; exchange replies `CancelRejected` | Order transitions cleanly to `Filled`; `CancelRejected` clears `cancel_requested` without reverting terminal `Filled` state | `test_failure_recovery` |
| **Stale or out-of-sync market** | Quote age > `stale_threshold_ns` or `FeedHealth != Healthy` | `PreTradeRiskGate` rejects new orders with `RiskStaleMarket`; cancels remain permitted | `test_failure_recovery`, `test_multicast_spsc` |
| **Invalid price / price band / oversize / notional** | Zero price, >2% off mid, qty > max, notional > max | Rejected locally before gateway dispatch with exact `RejectReason` | `test_failure_recovery::RiskGateNegativeCasesAndKillSwitch` |
| **Duplicate client token** | Same `ClientToken` submitted twice | Rejected locally with `DuplicateToken` (and idempotently guarded at exchange) | `test_failure_recovery` |
| **Venue outage / kill switch** | Venue marked down or kill switch engaged | New orders blocked (`RiskVenueDown` / `RiskKillSwitch`); risk-reducing cancels still allowed when venue is reachable; SOR fails over to healthy venue | `test_failure_recovery`, `test_multi_venue_strategies` |
| **Multicast drops, corruption, duplication, reordering** | 15–20% drop, 10% CRC corruption, 15% duplicate, 20% reorder | CRC rejects corrupted packets; reorder buffer absorbs in-window reordering; gaps trigger retransmit or snapshot fallback; final state hash matches publisher | `test_multicast_spsc` |
| **SPSC queue saturation** | Bounded queue capacity reached | `Reject` returns `QueueFull`; `Backpressure` spins/yields; `DropAndResync` drops **only** `MarketDataEvent` (raising `resync_required`) and **never** drops `OrderIntent` or `ExecutionReport` | `test_multicast_spsc::SpscQueueOverflowPoliciesNeverSilentlyDropOrdersOrExecs` |

---

## 3. Fault-Injection Case Study: Disconnect + Duplicate Fill + Cancel/Fill Race

From `test_failure_recovery.cpp` (`DuplicateFillsAndReconnectExposureConservation`):

1. **Initial state:** `max_long_position_lots = 120`. Strategy submits `REC00000000001` (Buy 80 @ `$100.00`). `OmsReconciler` immediately counts 80 lots in `pending_buy_lots` while `PendingNew`.
2. **Disconnect before Ack:** `on_session_disconnect()` fires at `t = 2,000 ns`. `REC00000000001` transitions to `Uncertain`, retaining its full 80-lot worst-case buy exposure.
3. **Invariant enforced during outage:** Strategy attempts a second Buy 50 (`REC00000000002`). Worst-case long exposure = `0 + 80 (Uncertain) + 50 = 130 > 120`. `PreTradeRiskGate` rejects with `RiskMaxPosition`, preventing silent position limit breach while order 1's exchange status is unknown.
4. **Reconnect & idempotent reconciliation:** On reconnect (`t = 3,000 ns`), sequenced exchange replay delivers:
   - `Accepted` (`REC00000000001`, exchange ref 5001)
   - `PartiallyFilled` (30 lots, `exec_id = 777`)
   - **Duplicate** `PartiallyFilled` (30 lots, same `exec_id = 777`)
   
   The reconciler applies the ack and first fill, ignores the duplicate (`duplicate_reports_ignored = 1`), updates confirmed position to **30** (not 60!), and lowers `pending_buy_lots` to **50**.
5. **Cancel/fill race:** Strategy submits Cancel for the remaining 50 lots (`t = 4,000 ns`). Exposure stays 50 while `PendingCancel`. Before the cancel reaches the exchange, the remaining 50 lots fill (`exec_id = 778`), followed by `CancelRejected`. The order terminates as `Filled` (80/80 filled, 0 leaves).
6. **Durable journal verification:** Project 03's `ouch::EventLog` binary journal (`/tmp/hft_test_recovery.journal`) replays into a fresh `ouch::OrderManager` with zero decode errors and zero invariant violations, and `AccountingLedger::reconcile()` reports `discrepancy_scaled == 0`.

---

## 4. Operational Recovery Runbook

1. **On Feed Gap (`FeedHealth::GapDetected` / `Resyncing`):**
   - Block all new risk-increasing orders automatically via `CanonicalOrderBook::is_tradable() == false`.
   - Buffer out-of-order packets within `reorder_window_packets`.
   - Request TCP retransmit (`MDR1 RETRANSMIT_REQ`); if range exceeds retransmit ring (`Unavailable`), request checkpoint snapshot (`MDR1 SNAPSHOT_REQ`), apply snapshot at `as_of_seq = N`, discard buffered packets `<= N`, replay contiguous packets `> N`, then restore `FeedHealth::Healthy`.
2. **On Gateway Disconnect (`SessionDown`):**
   - Transition all non-terminal venue orders to `Uncertain` while retaining worst-case exposure.
   - Do **not** generate fresh client tokens for uncertain orders.
   - Reconnect via SoupBinTCP requesting `next_seq` from the binary `EventLog`; reconcile sequenced outbound messages first, and only resend unacknowledged requests under their **original** idempotent `ClientToken` (`ouch::ReconcilePolicy::ResendSameToken`).
3. **On SPSC Overflow (`ErrorCode::FeedGap` or `ErrorCode::QueueFull`):**
   - For market-data SPSC under `DropAndResync`, immediately mark downstream books `Resyncing` and trigger snapshot refresh.
   - For order/execution SPSC, never drop: apply `Backpressure` or fail fast with `Reject` (`QueueFull`) and escalate to kill switch if sustained.
4. **On Ledger Discrepancy (`reconcile().ok == false`):**
   - Engage kill switch (`PreTradeRiskGate::set_kill_switch(true)`), cancel all resting quotes, dump execution history + binary `EventLog`, and halt new trading until reconciled.
