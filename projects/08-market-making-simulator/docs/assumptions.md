# Explicit Assumptions, Execution Models, and Limitations

This repository is an educational portfolio and research simulator. Every execution and market-structure simplification is documented explicitly below.

## Execution Model Assumptions (Modeled, Not L3 Exchange Queue Data)

1. **Queue-Priority Model (`queue_priority`, default)**:
   - Resting simulated orders join at the back of the visible aggregate L2 depth at their price level (`queue_ahead = book.level_qty(side, price)` at exchange arrival time).
   - Market trades at the resting price first deplete `queue_ahead`; fills only occur on excess trade volume beyond `queue_ahead`. Not every touch of a resting quote results in a fill.
   - Non-trade level shrinks (`BookDelta` reductions not attributable to a contemporaneous `Trade`) reduce `queue_ahead` by `queue_cancel_ahead_fraction` (default `0.50`), clamped to the remaining visible depth.
   - Trades or quotes that move strictly *through* a resting limit price fill the resting order passively at its limit price.
   - **Limitation**: Without real L3 order-by-order queue telemetry, the fraction of cancellations occurring ahead of vs. behind our queue position is an assumption (`0.50`), not an exchange measurement.

2. **Probabilistic Touch Model (`probabilistic_touch`, sensitivity alternative)**:
   - On trades at the resting price, fills occur with probability `touch_fill_prob * distance_decay_per_tick^dist`, where `dist` is the tick distance from top-of-book.
   - Included explicitly to measure strategy sensitivity to fill-model assumptions (`results/experiments/sweep_fill_model.csv`).

3. **No Market Impact / Reactive Counterparties**:
   - Simulated quotes do not alter subsequent replayed market events (counterparties do not react to our resting size, nor do our cancels trigger other participants' algorithms).
   - Consequently, simulated fill rates and P&L at large order sizes (`base_quote_qty`) are upper-bound approximations relative to live venues.

4. **Deterministic Constant Latency**:
   - `md_latency_ns`, `order_latency_ns`, and `ack_latency_ns` are modeled as fixed constants per run (swept across `0 us` to `20 ms` in robustness studies) rather than heavy-tailed jitter distributions.

5. **Synthetic Data Regimes**:
   - The 12-session study uses a deterministic synthetic L2 + informed/uninformed trade generator (`src/synth.cpp`) spanning `mean_reverting`, `toxic_trend`, `high_vol_burst`, and `thin_book` regimes.
   - Synthetic results demonstrate strategy mechanics, risk controls, and sensitivity behaviors; they are **not** evidence of live trading profitability on real exchange feeds.

6. **Performance & Risk Metrics**:
   - Sharpe ratios are intentionally omitted unless explicitly normalized over multi-day live periods with clear sampling and annualization conventions.
   - Adverse-selection markouts at `10 ms`, `100 ms`, and `1 s` are computed **only** for fills with `fill_time + horizon <= session_end_time` (never extrapolated).
