# Data Contracts & Schemas (`mmae.v1`)

**Schema Version:** `mmae.v1`  
**Scope:** Canonical input events, engine configuration, causal feature output, replay diagnostics, and offline evaluation output.

---

## 1. Units, Identifiers, and Timestamp Semantics

| Field / Concept | Type | Unit / Semantics | Validation Rule |
|---|---|---|---|
| `event_ts_ns` | `int64` | Exchange / event-time nanoseconds since Unix epoch | Must be `> 0`. Must be monotonically non-decreasing (`event_ts_ns >= prev_event_ts_ns`); violations are flagged and rejected as `OUT_OF_ORDER_TS`. |
| `recv_ts_ns` | `int64` | Local capture timestamp in nanoseconds since Unix epoch | Must be `>= 0`. Informational only; causal ordering uses `event_ts_ns` and `seq_num`. |
| `seq_num` | `uint64` | Monotonic per-symbol feed sequence number | Must be `>= 1`. `seq_num == prev_seq + 1` is normal; `seq_num > prev_seq + 1` opens/extends a **sequence gap** (`gap_detected_here = true`); `seq_num <= prev_seq` is rejected as `DUPLICATE_OR_REGRESSING_SEQ`. |
| `symbol` | `string` | Instrument identifier (ASCII, no whitespace/commas) | Non-empty. Single-symbol per replay instance; mismatched symbols are rejected as `SYMBOL_MISMATCH`. |
| `tick_size` | `float64` | Quote currency per integer tick (e.g., `0.01`) | Finite and `> 0`. Prices are keyed internally in integer ticks (`price_ticks`) to eliminate floating-point key drift. |
| `price_ticks` | `int64` | Integer multiple of `tick_size` | Must be `> 0` for price-bearing events (`L2_SET`, `L3_ADD`, `L3_MODIFY`, `L3_EXECUTE`, `TRADE`); `0` allowed for `L2_SNAPSHOT_CLEAR`, `L3_CANCEL`, `HEARTBEAT`. |
| `qty` | `int64` | Integer shares / contracts / lots | Must be `>= 0` for `L2_SET`, `L3_MODIFY`, `L3_CANCEL`, `L2_SNAPSHOT_CLEAR`, `HEARTBEAT`; must be `> 0` for `L3_ADD`, `L3_EXECUTE`, `TRADE`. |
| `order_id` | `uint64` | Exchange order identifier for L3 queue tracking | Required `> 0` for `L3_ADD`, `L3_MODIFY`, `L3_CANCEL`, `L3_EXECUTE`; `0` for L2/TRADE/HEARTBEAT. |

---

## 2. Canonical Input Event CSV Schema

File format: UTF-8 CSV with exact header row:

```csv
event_ts_ns,recv_ts_ns,seq_num,symbol,event_type,side,price_ticks,qty,order_id
```

### Supported `event_type` values

| `event_type` | `side` | `price_ticks` | `qty` | `order_id` | Semantics |
|---|---|---|---|---|---|
| `L2_SNAPSHOT_CLEAR` | `UNKNOWN` (or `BID`/`ASK`) | `0` | `0` | `0` | Clears the full book (or specified side if `BID`/`ASK`). Clears active `gap_active` state when `clear_gap_on_snapshot=true`. |
| `L2_SET` | `BID` or `ASK` | `> 0` | `>= 0` | `0` | Sets aggregate level quantity at `(side, price_ticks)`. `qty == 0` removes the level. |
| `L3_ADD` | `BID` or `ASK` | `> 0` | `> 0` | `> 0` | Appends order `order_id` to the tail of the FIFO queue at `(side, price_ticks)`. |
| `L3_MODIFY` | `BID` or `ASK` | `> 0` | `>= 0` | `> 0` | Modifies `order_id`. Price change or quantity increase loses FIFO priority (moves to tail); quantity decrease at same price preserves FIFO priority; `qty == 0` deletes order. |
| `L3_CANCEL` | `BID`, `ASK`, or `UNKNOWN` | `>= 0` | `>= 0` | `> 0` | Reduces `order_id` by `qty` (or deletes entirely if `qty == 0` or `qty >= remaining_qty`). |
| `L3_EXECUTE` | `BID`, `ASK`, or `UNKNOWN` | `>= 0` | `> 0` | `> 0` | Executes `qty` against resting `order_id`. Records pre-execution FIFO queue metrics (`exec_queue_ahead_qty`, `exec_queue_ahead_orders`, `exec_queue_position_ratio`, `fifo_anomaly`). |
| `TRADE` | `UNKNOWN` (ignored) | `> 0` | `> 0` | `0` | Tape print classified causally against the **pre-trade** book state with explicit ambiguity. |
| `HEARTBEAT` | `UNKNOWN` | `0` | `0` | `0` | Advances `event_ts_ns` without updating quotes (allows staleness detection during quote droughts). |

---

## 3. Validation Error Codes

Invalid rows are **never silently accepted**. Each rejected event increments `diagnostics.rejected_events` and records a structured error entry `{line_number, seq_num, error_code, detail}`:

- `MALFORMED_ROW`: Wrong column count or unparseable integer.
- `UNKNOWN_EVENT_TYPE`: `event_type` not in the supported enum.
- `INVALID_SIDE`: `side` not in `{BID, ASK, UNKNOWN}` or `UNKNOWN` where side is required.
- `INVALID_TIMESTAMP`: `event_ts_ns <= 0` or `recv_ts_ns < 0`.
- `OUT_OF_ORDER_TS`: `event_ts_ns < last_event_ts_ns`.
- `DUPLICATE_OR_REGRESSING_SEQ`: `seq_num == 0` or `seq_num <= last_seq_num`.
- `SYMBOL_MISMATCH`: `symbol` empty or differs from stream symbol.
- `INVALID_PRICE`: `price_ticks <= 0` on price-bearing event.
- `INVALID_QTY`: `qty < 0` or `qty == 0` where positive quantity is required.
- `INVALID_ORDER_ID`: `order_id == 0` on L3 event.
- `BOOK_CAPACITY_LEVELS_EXCEEDED`: Bounded `max_levels` exceeded.
- `BOOK_CAPACITY_ORDERS_EXCEEDED`: Bounded `max_orders` exceeded.
- `BOOK_DUPLICATE_ORDER_ID`: `L3_ADD` with an already-active `order_id`.
- `BOOK_ORDER_NOT_FOUND`: `L3_MODIFY`/`L3_CANCEL`/`L3_EXECUTE` referencing unknown `order_id`.

---

## 4. Causal Feature Output CSV Schema (`mmae.v1`)

Emitted strictly in causal event order (one row per accepted event):

- **Identity & state:** `seq_num`, `event_ts_ns`, `recv_ts_ns`, `symbol`, `event_type`, `book_valid`, `is_crossed`, `is_locked`, `is_stale`, `gap_active`, `gap_detected_here`
- **Quotes & spread:** `best_bid_ticks`, `best_ask_ticks`, `best_bid_px`, `best_ask_px`, `mid_price`, `spread_ticks`, `spread_abs`, `spread_bps`, `microprice`, `microprice_offset_ticks`
- **Depth & imbalance:** `bid_qty_l1`, `ask_qty_l1`, `bid_depth_k`, `ask_depth_k`, `obi_l1`, `obi_k`, `ofi_event`, `ofi_window`
- **Trade classification & signed flow:** `is_trade`, `trade_price_ticks`, `trade_price`, `trade_qty`, `trade_sign`, `classification_reason`, `is_ambiguous`, `tick_rule_hint`, `signed_qty`, `rolling_signed_flow_qty`, `rolling_total_trade_qty`, `rolling_ambiguous_qty`, `rolling_trade_flow_imbalance`
- **Queue metrics:** `bid_l1_order_count`, `ask_l1_order_count`, `exec_queue_ahead_qty`, `exec_queue_ahead_orders`, `exec_queue_position_ratio`, `fifo_anomaly`, `top_bid_depletion_qty`, `top_ask_depletion_qty`, `cancel_to_add_ratio`

### Trade Classification Rules (`trade_sign` and `classification_reason`)

Evaluated strictly using the **pre-trade** causal book state:

1. If `gap_active == true` $\rightarrow$ `trade_sign = 0` (`AMBIGUOUS`), reason `GAP_ACTIVE`.
2. Else if missing bid or missing ask $\rightarrow$ `trade_sign = 0`, reason `BOOK_INCOMPLETE`.
3. Else if `best_bid_ticks > best_ask_ticks` $\rightarrow$ `trade_sign = 0`, reason `BOOK_CROSSED`.
4. Else if `best_bid_ticks == best_ask_ticks` $\rightarrow$ `trade_sign = 0`, reason `BOOK_LOCKED`.
5. Else if `event_ts_ns - last_quote_ts_ns > stale_threshold_ns` $\rightarrow$ `trade_sign = 0`, reason `BOOK_STALE`.
6. Else compare `2 * trade_price_ticks` against `best_bid_ticks + best_ask_ticks` (exact integer midpoint comparison):
   - `trade_price_ticks >= best_ask_ticks` $\rightarrow$ `trade_sign = +1` (`QUOTE_AT_OR_ABOVE_ASK`)
   - `trade_price_ticks <= best_bid_ticks` $\rightarrow$ `trade_sign = -1` (`QUOTE_AT_OR_BELOW_BID`)
   - `2 * trade_price_ticks > best_bid_ticks + best_ask_ticks` $\rightarrow$ `trade_sign = +1` (`QUOTE_ABOVE_MID`)
   - `2 * trade_price_ticks < best_bid_ticks + best_ask_ticks` $\rightarrow$ `trade_sign = -1` (`QUOTE_BELOW_MID`)
   - `2 * trade_price_ticks == best_bid_ticks + best_ask_ticks` $\rightarrow$ `trade_sign = 0` (`MIDPOINT_AMBIGUOUS`, `is_ambiguous = 1`).
7. Independently, `tick_rule_hint` records `+1` if `trade_price_ticks > prev_trade_price_ticks`, `-1` if `<`, or carries the last non-zero tick direction if equal (`0` if none). It **never** silently overwrites `trade_sign = 0`.

---

## 5. Diagnostics & Evaluation JSON Contracts

- **Replay diagnostics (`diagnostics.json`):** Contains `schema_version`, `run_id`, `engine_variant`, `config`, `counts`, `book_health`, `trade_classification`, `queue_metrics`, `gaps[]` (every detected gap with `expected_seq`, `observed_seq`, `missing_count`, timestamps, and snapshot resolution status), and `validation_errors[]`.
- **Chronological evaluation (`evaluation.json`):** Contains `schema_version`, `backend` (`polars` or `stdlib`), `input_sha256`, `split` (chronological IS/OOS boundary + embargo duration), `coverage` (valid non-gap/non-stale/non-ambiguous ratios), `price_response_by_horizon`, `adverse_selection_by_horizon` (verifying $s_{\text{eff}} = s_{\text{real}}(h) + \text{AS}(h)$), and `oos_predictive_stats` (linear model trained strictly on IS vs naive zero baseline evaluated on post-embargo OOS).

---

## 6. Fixture Catalog

| Fixture | Category | Purpose |
|---|---|---|
| `fixtures/hand_worked_l3.csv` + `fixtures/hand_worked_l3_expected.json` | Valid deterministic reference | 12-event L3 + TRADE sequence with exact hand-calculated spread, microprice, OBI, OFI, FIFO queue ahead, and midpoint ambiguity. |
| `fixtures/edge_cases_crossed_stale_gaps.csv` | Boundary & edge cases | Crossed book, locked book, heartbeat-induced stale quote, multiple sequence gaps with snapshot recovery, and ambiguous trade reasons. |
| `fixtures/invalid_events.csv` | Invalid inputs | Malformed columns, unknown event types, negative qty, zero price, out-of-order timestamp, regressing sequence, missing order ID. |
| `fixtures/synthetic_signed_flow_config.json` | Integration scenario | Fixed-seed (`seed=42`) configuration generating a multi-thousand-event synthetic L3+TRADE stream with known persistent signed order flow and short-horizon price impact. |
