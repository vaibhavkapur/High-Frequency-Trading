# Design

How the simulator is put together, and why. Assumptions and known gaps of the fill model are in
[assumptions.md](assumptions.md).

## Components

```text
                     exchange time                  │            strategy time
                                                    │
 feed (CSV/bin) ──► Exchange ──► truth OrderBook    │
   sorted by ts       │  ▲                          │
                      │  └── simulated orders       │
                      │      (queue position)       │
                      │                             │
                      ├─ market event ──[md path]───┼──► observed OrderBook ──► Strategy::on_market_data
                      │                             │                                │
                      ├─ exec report ──[resp path]──┼──► Oms (observed) ──► Strategy::on_order_update
                      │                             │                                │
                      ▼                             │                                ▼
                  Portfolio (truth)                 │                    Context::send_limit / cancel
                  markouts, equity                  │                         │  pre-trade risk (Oms)
                                                    │                         ▼
                  Exchange ◄──[order path]──────────┼───────────────── compute delay
```

| Component | File | Responsibility |
|---|---|---|
| `Engine` | `src/engine.cpp` | Event loop, scheduling, the only owner of "now" |
| `Exchange` | `src/exchange.cpp` | Truth book per instrument, fill model for simulated orders |
| `OrderBook` | `src/order_book.cpp` | L2 levels + optional L3 order map; used for truth *and* the delayed view |
| `LatencyPath` | `include/hftbt/latency.hpp` | One ordered link: base delay + seeded jitter, never reorders |
| `Oms` | `src/oms.cpp` | Strategy-side order state, position and pre-trade risk, built from *observed* reports |
| `Portfolio` | `src/portfolio.cpp` | Cash, inventory, fees, mark-to-market at exchange time |
| `Strategy` / `Context` | `include/hftbt/strategy.hpp` | The only surface a strategy can touch |
| feed I/O | `src/feed.cpp` | CSV and binary loaders with full validation |
| `generate_l3` | `src/synth.cpp` | Seeded synthetic order flow for tests and benchmarks |

## Two clocks, two books

Every instrument has two `OrderBook` instances:

- **truth** (inside `Exchange`): updated when a feed event's exchange timestamp is reached. Fills are
  decided against this book.
- **observed** (inside `Engine`): updated when the same event is *delivered* to the strategy, i.e. at
  `ts + md_delay (+ jitter)`.

`Context::book()` returns the observed book and `Context::position()` returns the position implied by
exec reports that have already arrived. A strategy has no handle on the truth book, the truth
position or any future event, so lookahead is not something a strategy author has to remember to
avoid — there is nothing to look at. `tests/test_causality.cpp` asserts the two views differ in
exactly the expected way at specific nanoseconds.

## Events and deterministic ordering

All times are `int64` nanoseconds. One run processes these event kinds:

| Priority | Kind | When it is scheduled |
|---|---|---|
| 0 | Feed event reaches the exchange | its exchange timestamp |
| 1 | New order / cancel arrives at the exchange | send time + compute delay + order-path latency |
| 2 | Market data observed by the strategy | exchange timestamp + market-data latency |
| 3 | Exec report observed by the strategy | exchange event time + response latency |
| 4 | Strategy timer | requested time (clamped to now) |
| 5 | Markout measurement | fill time + horizon |
| 6 | Equity sample | every `sample_interval_ns` |

Events are dispatched in order of `(timestamp, priority, sequence)`, where `sequence` is a counter
assigned when the event is scheduled. There is no other source of ordering: no wall clock, no
threads, no hash-map iteration, and the only random numbers (latency jitter) come from a seeded
xoshiro256** generator. Same feed + same config + same seed therefore gives the same sequence of
dispatched events, which the engine condenses into `event_hash` (every dispatched event plus the
final cash and positions).

Two consequences of the priority order are worth stating because they are modelling choices:

- **Feed before orders at the same nanosecond.** If an order arrives at exactly the timestamp at
  which a quote is cancelled, the quote is already gone. This is the pessimistic choice.
- **Nothing is scheduled in the past.** Every latency is clamped to ≥ 0 and each `LatencyPath` is
  monotone, so an event handler can only create events at `now` or later.

The run ends at the last feed timestamp (or `run.end_ts`). Events scheduled after that — an order
still in flight, market data not yet delivered — are dropped, and `summary.json` reports
`open_orders_at_end` and `markouts_unresolved`.

### Scheduler data structures

Feed events are already sorted, so they are never put in the heap: the loop does a three-way merge
between the next feed event, a binary heap of dynamic events, and a FIFO of market data in flight to
the strategy. The FIFO is valid because the market-data path is monotone (deliveries are
non-decreasing even with jitter). `run.md_queue = heap` routes delayed market data through the heap
instead; `Scheduler.FifoAndHeapMarketDataQueuesAreEquivalent` proves both produce the same
`event_hash`, and [benchmarks.md](benchmarks.md) measures the difference.

## Order lifecycle

```text
strategy                         exchange
   │ send_limit()                    │
   ├─ risk check (Oms) ── reject ──► returns {id 0, reason} immediately, nothing is sent
   │  PendingNew                     │
   │ ───── compute + order path ────►│ validate, post-only check
   │                                 │ ACK
   │                                 │ taker part: walk displayed levels within limit ── FILL (taker)
   │                                 │ remainder: IOC/market → CANCELLED, else rests with
   │                                 │            ahead = displayed qty at its price
   │ ◄──────── response path ────────│
   │  Live                           │ historical trades / cancels move the queue
   │                                 │ FILL (maker) when the queue ahead is exhausted
   │ cancel()                        │
   │  PendingCancel ── order path ──►│ still resting → CANCELLED
   │                                 │ already gone  → CANCEL_REJECTED (too late); fills stand
```

Position and open quantity in the `Oms` change only when a report is *observed*. Pre-trade risk
checks the worst case — observed position plus every open order on that side plus the new order —
against `risk.max_position`, which bounds the true position even while fills are still in flight
(asserted in the fuzz test).

## Fill model

Simulated orders never change the historical book (no market impact). What they need is an answer
to "would this order have traded?", derived from historical events at or through its price.

### Resting orders

Each resting simulated order carries `ahead`: historical quantity that must trade before it.

| Feed | Event at our price level | Effect |
|---|---|---|
| L3 | order that was there before us cancels / reduces | `ahead -= qty` (exact) |
| L3 | order that arrived after us cancels | nothing (exact) |
| L3 | order ahead of us executes | `ahead -= qty` |
| L3 | order **behind** us executes | we would have traded first: fill up to the executed qty |
| L2 | trade print of `q` | `ahead -= q`; any excess over `ahead` fills us |
| L2 | level decreases by more than recent trades explain | a cancellation of unknown position — see queue models |
| any | trade at a price worse than ours (trade-through) | fill up to the traded qty |
| any | new liquidity posted on the other side at/through our price | fill up to the posted qty (`fill_on_crossing_quote`) |

With an L3 feed nothing is approximated: "ahead of us" is decided by comparing the arrival sequence
of the historical order with the sequence at which our order joined.
`Property.L3QueuePositionMatchesBruteForceFifoModel` checks this against an independent brute-force
model on 300 random scenarios.

With an L2 feed the position of a cancellation inside the queue is unknowable, so it is a
configuration choice (`exchange.queue_model`):

| Model | Assumption | Bias |
|---|---|---|
| `conservative` (default) | cancels come from behind us whenever possible | fewest fills |
| `proportional` | cancels are spread uniformly over the queue (rounded against us) | middle |
| `optimistic` | cancels come from ahead of us first | most fills |

In every model `ahead` is clamped to the displayed quantity, and a cancellation alone never fills
an order.

One historical trade is never used twice: if several of our orders are eligible, they share the
traded quantity in price-time priority.

### Aggressive orders

A marketable order walks the displayed levels of the truth book, best price first, within its
limit. Each level keeps a `consumed` counter so that our own orders cannot take the same displayed
lots twice; a historical execution at that level releases it (those lots are gone for everyone),
other decreases cap it at what is still displayed. `exchange.taker_slippage_ticks` adds a fixed
adverse offset to every taker fill as a crude stand-in for impact.

### Fees and accounting

Fees are basis points of traded notional, separately for maker and taker fills (negative = rebate).
`Portfolio` keeps cash and inventory per instrument and marks inventory at the truth mid at each
equity sample. Two independent conventions are computed — `cash + position × mark` and
`realized + unrealized − fees` (average cost) — and tested to agree, including on fuzzed input.

### Adverse selection (markouts)

For every fill and every configured horizon `h`, the engine records
`side × (mid(t_fill + h) − fill_price)` using the truth mid, split by maker/taker and averaged per
lot. Negative maker markouts mean the quotes were picked off.

## Feed formats

CSV, one event per line, integers only:

```text
ts_ns,symbol,type,order_id,side,price,qty
```

| Type | Meaning | Fields used |
|---|---|---|
| `A` | add resting order (L3) | order_id, side, price, qty |
| `C` | reduce resting order by qty (L3) | order_id, qty |
| `D` | delete resting order (L3) | order_id |
| `E` | resting order executed for qty (L3) | order_id, qty |
| `L` | set aggregate qty at a price level; 0 removes it (L2) | side, price, qty |
| `T` | trade print; side is the **aggressor** (L2) | side, price, qty |

Prices are integer ticks and quantities integer lots; `python -m hftbt_tools.feedio` converts
Parquet with decimal prices given a tick size, and refuses prices that are not whole ticks. An
order replace is expressed as `D` + `A` (loses queue priority, as on most venues).

### Binary feed format

Little-endian, no padding. Loaded by `parse_bin`, which bounds-checks every field.

```text
char[8]  magic      "HFTBTFD1"
u32      version    1
u32      n_symbols
u64      n_events
n_symbols × { u16 len (1..64); char[len] }
n_events  × { i64 ts; u64 order_id; i64 price; i64 qty; u32 instrument; u8 type; u8 side }   38 bytes
```

The file must end exactly after the last record. Both loaders then apply the same semantic checks
(timestamps non-negative and non-decreasing, instrument in range, known type, price and quantity in
range). Events that pass validation but contradict the book — unknown order id, duplicate id —
are skipped and counted in `data_anomalies` rather than trusted.

## Worked example

`examples/configs/tiny_demo.ini` replays 13 hand-written events with 100 µs market-data latency and
200 µs each way on the order path. The strategy joins the best bid and ask with 4 lots as soon as it
sees a two-sided book. Excerpt of `results/tiny_demo/events.log`:

```text
1000000 FEED DEMO A id=1 side=B px=9999 qty=10     book: 10 @ 9999 / 10 @ 10001
1100000 MD_OBSERVED DEMO A exch_ts=1000000         strategy sees it 100 µs later, sends 2 orders
1300000 ORDER_ARRIVE id=1 DEMO side=B px=9999 qty=4   200 µs later: joins behind 10 lots
1500000 FEED DEMO A id=3 side=B px=9999 qty=5      another order joins BEHIND us
1500000 REPORT_OBSERVED id=1 ACK exch_ts=1300000   ack observed 200 µs after it was issued
3000000 FEED DEMO E id=1 qty=10                    the 10 lots ahead trade: ahead 10 → 0, no fill
3200000 FEED DEMO C id=3 qty=2                     cancel behind us: no effect
3500000 FEED DEMO E id=3 qty=3                     the order behind us trades 3 ...
3500000 EXCH_FILL id=1 px=9999 qty=3 leaves=1 maker   ... so we must have traded those 3 first
3700000 REPORT_OBSERVED id=1 FILL exch_ts=3500000  position becomes +3 for the strategy only now
5000000 EXCH_FILL id=1 px=9999 qty=1 leaves=0 maker
```

Meanwhile the 4-lot ask at 10001 sits behind 10 historical lots; only 6 of them trade (at
8000000), so it ends the run unfilled with 4 lots still ahead.
