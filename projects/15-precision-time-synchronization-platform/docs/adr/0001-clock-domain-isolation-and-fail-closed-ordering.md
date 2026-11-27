# ADR 0001: Strict Clock-Domain Isolation and Fail-Closed Causal Ordering

- **Status:** Accepted
- **Date:** 2026-11-27
- **Project:** Precision Time Synchronization Platform (`ptsp`)

## Context

In distributed market-data systems, timestamps originate from heterogeneous clocks: grandmaster-disciplined `TAI`, leap-second-aware `UTC`, local `MONOTONIC_RAW` oscillators, and venue-assigned clocks. Silent mixing of domains (for example, subtracting a `UTC` wall timestamp from a `TAI` PTP timestamp, which differ by 37 seconds) or emitting events in an arbitrary guessed order when clock uncertainty exceeds inter-event spacing corrupts downstream microstructure analytics and backtests without visible errors.

Similarly, assuming NIC hardware PTP accuracy on hosts where hardware timestamping is unavailable produces misleading sub-microsecond claims.

## Decision

1. **Typed Provenance & Zero Silent Domain Mixing**: Every timestamp carries a mandatory `ClockProvenance` (`domain`, `role`, `capture_method`, `error_source`, `clock_id`, `epoch_id`). Both `RobustSyncEstimator` and `FailClosedOrderer` lock onto their configured clock domain pair and immediately return `ErrorCode::ClockDomainMismatch` on any mismatch.
2. **Explicit Separation of Error Sources**: Every diagnostic and report explicitly tags whether error/latency numbers are `MEASURED` (host OS/hardware probe), `SYNTHETIC_FIXTURE` (deterministic simulation), or `ESTIMATED_MODEL` (estimator output).
3. **Fail-Closed Ordering**: `FailClosedOrderer` halts into `OrderingState::HaltedFailClosed` whenever:
   - clock uncertainty exceeds `max_skew_ns`,
   - aligned receive-time precedes event-time beyond uncertainty + causal slack (`CausalViolation`),
   - a late packet arrives behind the already-emitted event watermark, or
   - two cross-source events have overlapping `[t - u, t + u]` uncertainty intervals (`AmbiguousOrdering`).
   While halted, all subsequent packets and flushes are rejected until explicit operator recovery (`recover_and_reset_buffer()`).
4. **Honest Hardware Capability Probing**: `LinuxPtpHardwareAdapter` checks actual Linux PHC / `ETHTOOL_GET_TS_INFO` capabilities and returns `HardwareTimestampUnavailable` on unsupported hosts or NICs rather than falling back silently to software timestamps under a hardware label.

## Consequences

- **Positive**: Eliminates silent domain bugs, makes uncertainty and hardware limitations auditable, and guarantees monotonic, causally validated output streams.
- **Negative / Trade-off**: Requires callers and fixtures to supply explicit provenance metadata and introduces halted states during severe skew faults that require explicit operator recovery.
