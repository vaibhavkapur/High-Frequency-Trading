# Assumptions and limitations

A backtest is a model. This page lists what this one assumes, the direction each assumption is
likely to bias results, and what it does not attempt at all.

## What is assumed

| # | Assumption | Likely bias | Knob |
|---|---|---|---|
| 1 | **No market impact.** Simulated orders never alter the historical book or other participants' behaviour. | Optimistic, increasingly so with size relative to displayed depth. | `exchange.taker_slippage_ticks` (crude) |
| 2 | **Displayed liquidity is all there is.** Hidden/iceberg quantity ahead of us is invisible in the feed and treated as absent. | Optimistic for queue position; pessimistic for taker fills against hidden size. | none |
| 3 | **L3 queue position is exact; L2 is not.** With market-by-price data the position of each cancellation is unknown. | Depends on the model: `conservative` under-fills, `optimistic` over-fills. | `exchange.queue_model` |
| 4 | **L2 trade prints precede the depth update they cause.** A level decrease is attributed to trades printed since the last update of that level, the rest to cancellations. | If a venue publishes depth first, `ahead` is decremented twice (optimistic). | reorder in preprocessing |
| 5 | **A trade behind us implies we traded.** If history shows an execution of an order that would have been behind ours, we are filled first, up to the historical trade size. | Roughly neutral for small orders; ignores that the aggressor might not have sent the order had the book looked different. | none |
| 6 | **Crossing quotes fill us.** Liquidity posted on the other side at or through our resting price trades with us. | Mildly optimistic (the poster may have priced off our quote). | `exchange.fill_on_crossing_quote` |
| 7 | **Our own taker volume is remembered per level** until history trades through it, but the strategy's *observed* book does not show the hole. | Conservative: a strategy may fire at liquidity it already took and get nothing. | none |
| 8 | **Latency is base + uniform jitter per path, ordered.** No bursts, no loss, no correlation with market activity. | Optimistic exactly when it matters (latency spikes coincide with busy markets). | `[latency]` section |
| 9 | **Ties go to the market.** At equal timestamps, historical events are applied before simulated orders. | Conservative. | none |
| 10 | **Fees are proportional** to notional with separate maker/taker rates. No tiers, minimums or per-order fees. | Either way. | `[fees]` section |
| 11 | **Marking at the mid.** Inventory is marked at the truth mid (last valid mid if one side is empty). No liquidation cost is charged for the final position. | Optimistic for large end-of-run inventory. | none |
| 12 | **Exposure and drawdown are sampled**, not tracked per event. | Under-states extremes between samples. | `run.sample_interval_ns` |

## What is not modelled

- Order modify/replace (use cancel + new), stop orders, auctions, halts, self-trade prevention.
- Exchange-side risk checks, throttles, message limits or rejections other than malformed orders
  and post-only crosses.
- Multiple venues, routing, or cross-instrument margin. Instruments share one event clock and one
  account but are otherwise independent.
- Real protocol decoding. Input must already be normalised to the CSV/binary schema in
  [design.md](design.md).
- Funding, borrow costs, corporate actions, currency conversion.

## What the bundled results do and do not show

Every dataset in this repository is **synthetic**, produced by `hftbt gen` from a seed. The flow is
uninformed by construction: order arrivals, cancellations and trade directions are independent
random draws. Two consequences:

- The market-making baseline earns the spread without being adversely selected (its markouts are
  positive). That is a property of the generator, not evidence that the strategy works.
- Sensitivity results (P&L vs latency, fees, queue model) demonstrate that the simulator responds
  to its assumptions in the expected direction. Their magnitudes do not transfer to any market.

Results on real data would need order-level history from a venue, and even then a profitable
backtest is a hypothesis to test in production with small size, not a forecast. `hftbt_tools.sweep`
always reports in-sample and out-of-sample separately, with the number of trials, for the same
reason.
