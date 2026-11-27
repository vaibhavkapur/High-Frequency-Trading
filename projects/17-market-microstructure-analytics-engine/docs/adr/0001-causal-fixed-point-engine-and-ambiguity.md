# ADR 0001: Causal Event-Time Engine, Fixed-Point Ticks, and Explicit Trade Ambiguity

- **Status:** Accepted
- **Date:** 2026-11-27
- **Context:** Project 19 — Market Microstructure Analytics Engine (`MMAE`)

## Context

Microstructure analytics often suffer from three recurring sources of subtle error:

1. **Lookahead leakage:** Joining trades to quotes using nearest-timestamp or batch window joins accidentally pulls quotes that arrived *after* the trade or feature timestamp.
2. **Floating-point price drift:** Using IEEE-754 doubles as order book price keys creates rounding mismatches when matching L3 modifies/cancels or aggregating L2 levels around tick boundaries.
3. **Silent trade-sign imputation:** Heuristic trade-sign classifiers (e.g., aggressive Lee-Ready fallback) silently assign `+1` or `-1` even when the trade prints at the exact midpoint, during a crossed/stale book, or during a sequence gap, distorting adverse-selection and order-flow statistics.

## Decision

1. **Strict single-pass causal state machine (`FeatureEngine`):**
   - Events are processed in strictly non-decreasing event time (`event_ts_ns`).
   - All feature outputs for event $i$ depend *exclusively* on events $0 \dots i$ (and never on event $j > i$).
   - Forward-looking labels ($\Delta m(t, h)$ for price response and adverse selection) are computed only in an isolated offline evaluation layer (`python/mmae_analytics/evaluator.py`) with a chronological train/test split and an explicit embargo of $\max(h)$ between in-sample and out-of-sample windows.

2. **Integer tick-indexed prices (`int64_t price_ticks`) and integer sizes (`int64_t qty`):**
   - Every event price is validated against the configured `tick_size` (or represented directly in integer ticks) so book keying, spread calculation, and parity checks between `baseline_map` and `optimized_flat` are exact.

3. **Explicit trade-sign ambiguity (`TradeSign::AMBIGUOUS`):**
   - Trade classification uses the causal pre-trade book state (quote rule).
   - If the book is incomplete, crossed, locked, stale, inside an active sequence gap, or if the trade prints strictly at the midpoint, the primary classification is explicitly `AMBIGUOUS` (`trade_sign = 0`) with a granular `ClassificationReason` enum.
   - An optional secondary `tick_rule_hint` (`+1` / `-1` / `0`) is recorded separately for diagnostic comparison without silently overwriting the primary `AMBIGUOUS` label.

4. **Dual book implementations with identical logical contract:**
   - `baseline_map`: `std::map` + `std::unordered_map` reference implementation prioritizing clarity.
   - `optimized_flat`: bounded contiguous level arrays + fixed-capacity L3 order slot pool avoiding hot-path heap allocations.
   - Both share the same `IOrderBook` interface and are continuously verified for bit-for-bit output parity on identical event streams.

## Consequences

- **Positive:** Lookahead leakage is structurally impossible in the C++ hot-path feature engine; ambiguity rates and gap windows are first-class reported metrics; baseline vs optimized benchmarking is apples-to-apples.
- **Negative / Trade-offs:** Strict midpoint/stale/gap ambiguity reduces classified trade coverage compared to aggressive imputation; bounded capacity in `optimized_flat` requires explicit sizing of `max_levels` and `max_orders` (overflows emit explicit errors rather than silently resizing).
