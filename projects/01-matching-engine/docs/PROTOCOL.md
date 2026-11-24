# Wire protocol

Two record types, both fixed-size and little-endian. A stream is a plain
concatenation of records with no header, framing or checksum. Integers are
two's complement. The authoritative definitions are in
[`include/me/protocol.hpp`](../include/me/protocol.hpp); the layout below is
checked by `Protocol.LayoutMatchesDocumentedOffsets` and
`Protocol.CommandEncodesToExactLittleEndianBytes`.

Prices and quantities are fixed-point integers. The unit is chosen by the venue
(for example 1 price unit = 0.0001 currency units); the engine only requires
prices to be multiples of the symbol's `tick_size` and quantities multiples of
its `lot_size`.

## Command (40 bytes)

| Offset | Size | Field | Type | Used by |
|---:|---:|---|---|---|
| 0 | 1 | `type` | u8 | all: 1 New, 2 Cancel, 3 Reduce, 4 Replace |
| 1 | 1 | `side` | u8 | New: 0 Buy, 1 Sell |
| 2 | 2 | `reserved` | u16 | ignored on input; write 0 |
| 4 | 4 | `symbol` | u32 | all: index of the symbol |
| 8 | 8 | `order_id` | u64 | New: id of the new order. Others: id of the resting order |
| 16 | 8 | `new_order_id` | u64 | Replace: id of the replacement order |
| 24 | 8 | `price` | i64 | New, Replace: limit price |
| 32 | 8 | `qty` | i64 | New, Replace: order quantity. Reduce: new remaining quantity |

Fields a command does not use are ignored. Any 40 bytes decode to a command;
invalid ones are rejected by the engine with a `Rejected` report, never dropped.
A stream whose length is not a multiple of 40 is truncated: `me_cli replay`
processes the complete records, reports the error and exits with status 2.

## Report (64 bytes)

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 8 | `seq` | u64, engine-wide, starts at 1, +1 per report |
| 8 | 8 | `order_id` | u64 |
| 16 | 8 | `other_id` | u64 |
| 24 | 8 | `price` | i64 |
| 32 | 8 | `qty` | i64 |
| 40 | 8 | `leaves` | i64 |
| 48 | 8 | `other_leaves` | i64 |
| 56 | 4 | `symbol` | u32 |
| 60 | 1 | `type` | u8: 1 Accepted, 2 Rejected, 3 Trade, 4 Canceled, 5 Reduced, 6 Replaced |
| 61 | 1 | `side` | u8: 0 Buy, 1 Sell |
| 62 | 1 | `reason` | u8: reject reason, 0 unless `type` is Rejected |
| 63 | 1 | `reserved` | u8, always 0 |

Field meaning by report type (unlisted fields are 0):

| Type | `order_id` | `other_id` | `side` | `price` | `qty` | `leaves` | `other_leaves` |
|---|---|---|---|---|---|---|---|
| Accepted | new order | – | order side | limit price | order quantity | order quantity | – |
| Rejected | command `order_id` | command `new_order_id` | command `side` byte | command price | command qty | – | – |
| Trade | taker (incoming) | maker (resting) | taker side | maker's price | traded quantity | taker remaining | maker remaining |
| Canceled | order | – | order side | order price | quantity removed | – | – |
| Reduced | order | – | order side | order price | quantity removed | new remaining | – |
| Replaced | old order | new order | order side | new price | new quantity | new quantity | old quantity removed |

Reports per command:

* `New` → `Accepted`, then zero or more `Trade`; or one `Rejected`.
* `Cancel` → `Canceled`; or `Rejected`.
* `Reduce` → `Reduced`; or `Rejected`.
* `Replace` → `Replaced`, then zero or more `Trade` for the replacement order; or `Rejected`.

An order is fully filled when a `Trade` reports `leaves == 0` (taker) or
`other_leaves == 0` (maker); there is no separate "filled" report.

Reject reasons:

| Value | Name | Meaning |
|---:|---|---|
| 1 | UnknownCommand | `type` is not 1–4 |
| 2 | UnknownSymbol | `symbol` is out of range |
| 3 | InvalidSide | `side` is not 0 or 1 |
| 4 | InvalidPrice | price ≤ 0 or above the symbol's `max_price` |
| 5 | InvalidTick | price is not a multiple of `tick_size` |
| 6 | InvalidQuantity | qty ≤ 0, above `max_qty`, not a multiple of `lot_size`, or a Reduce that does not lower the quantity |
| 7 | DuplicateOrderId | an order with that id is already resting on the symbol |
| 8 | UnknownOrderId | no order with that id is resting on the symbol |
| 9 | BookFull | the symbol already holds `max_orders_per_book` resting orders |

## Text formats

Used by `me_cli encode`, `me_cli replay --text-in/--text` and `me_cli dump`.
One command per line, whitespace separated, `#` starts a comment:

```text
N <symbol> <order_id> <B|S> <price> <qty>            new limit order
C <symbol> <order_id>                                cancel
R <symbol> <order_id> <new_remaining_qty>            reduce
X <symbol> <order_id> <new_order_id> <price> <qty>   cancel/replace
```

Reports are printed one per line as `key=value` pairs; see
[`examples/sample_session.expected.txt`](../examples/sample_session.expected.txt).
