# Protocol subset

This project is **OUCH-style**. It is modelled on two public NASDAQ specifications:

| Layer | Pinned version | Role |
|---|---|---|
| Order entry | **OUCH 4.2** | Message types and field layouts |
| Session / transport | **SoupBinTCP 3.00** | Framing, login, sequencing, heartbeats |

The OUCH offsets, lengths, price limits and "ignored without a response" rules below were
compared field by field on 2026-11-24 with the published OUCH 4.2 PDF (the revision marked
"Updated October, 2025") and matched. The SoupBinTCP section was written from the author's
reading of that specification and was **not** re-checked against the document. Nothing here has
been run against a NASDAQ certification environment or a captured production session, so this
code must not be treated as a certified or interoperable implementation.

All integers are unsigned big-endian. Alphanumeric fields are left-justified and space-padded on
the right. Prices are integers with four implied decimal places (`1500000` = $150.0000).
"Inbound" means client to exchange; "outbound" means exchange to client.

## Supported messages

### Inbound (sent as SoupBinTCP unsequenced data)

**Enter Order `O`: 49 bytes**

| Offset | Len | Field | Notes |
|---|---|---|---|
| 0 | 1 | Type | `O` |
| 1 | 14 | Order Token | Unique per account per day |
| 15 | 1 | Buy/Sell Indicator | `B` buy, `S` sell, `T` sell short, `E` sell short exempt |
| 16 | 4 | Shares | 1 to 999,999 |
| 20 | 8 | Stock | |
| 28 | 4 | Price | `0x7FFFFFFF` = market; otherwise at most 199,999.9900 |
| 32 | 4 | Time in Force | `0` = immediate-or-cancel, `99998` = market hours, `99999` = system hours |
| 36 | 4 | Firm | |
| 40 | 1 | Display | |
| 41 | 1 | Capacity | |
| 42 | 1 | Intermarket Sweep Eligibility | |
| 43 | 4 | Minimum Quantity | |
| 47 | 1 | Cross Type | |
| 48 | 1 | Customer Type | |

**Replace Order `U`: 47 bytes**

| Offset | Len | Field |
|---|---|---|
| 0 | 1 | Type `U` |
| 1 | 14 | Existing Order Token |
| 15 | 14 | Replacement Order Token |
| 29 | 4 | Shares |
| 33 | 4 | Price |
| 37 | 4 | Time in Force |
| 41 | 1 | Display |
| 42 | 1 | Intermarket Sweep Eligibility |
| 43 | 4 | Minimum Quantity |

**Cancel Order `X`: 19 bytes**

| Offset | Len | Field |
|---|---|---|
| 0 | 1 | Type `X` |
| 1 | 14 | Order Token |
| 15 | 4 | Shares |

### Outbound (sent as SoupBinTCP sequenced data)

Every outbound message starts with Type (1 byte) and Timestamp (8 bytes, nanoseconds since midnight).

| Message | Type | Bytes | Fields after the timestamp |
|---|---|---|---|
| System Event | `S` | 10 | Event Code (1) |
| Accepted | `A` | 66 | Token (14), Buy/Sell (1), Shares (4), Stock (8), Price (4), Time in Force (4), Firm (4), Display (1), Order Reference Number (8), Capacity (1), Intermarket Sweep (1), Minimum Quantity (4), Cross Type (1), Order State (1), BBO Weight Indicator (1) |
| Replaced | `U` | 80 | Replacement Token (14), Buy/Sell (1), Shares (4), Stock (8), Price (4), Time in Force (4), Firm (4), Display (1), Order Reference Number (8), Capacity (1), Intermarket Sweep (1), Minimum Quantity (4), Cross Type (1), Order State (1), Previous Order Token (14), BBO Weight Indicator (1) |
| Canceled | `C` | 28 | Token (14), Decrement Shares (4), Reason (1) |
| Executed | `E` | 40 | Token (14), Executed Shares (4), Execution Price (4), Liquidity Flag (1), Match Number (8) |
| Rejected | `J` | 24 | Token (14), Reason (1) |

Byte-exact fixtures for every message above live in `tests/test_wire.cpp`. Each fixture is spelled
out by hand from these tables, independently of the encoder, and must decode to the expected
fields and re-encode to the identical bytes.

### SoupBinTCP packets

Every packet is `length (2 bytes, big-endian, counts the type byte and payload)`, `type (1 byte)`,
`payload`.

| Type | Direction | Payload | Supported |
|---|---|---|---|
| `L` Login Request | client | Username (6), Password (10), Requested Session (10), Requested Sequence Number (20, ASCII digits, right-justified) | yes |
| `A` Login Accepted | server | Session (10), Sequence Number (20) | yes |
| `J` Login Rejected | server | Reason (1): `A` not authorized, `S` session not available | yes |
| `S` Sequenced Data | server | one OUCH outbound message | yes |
| `U` Unsequenced Data | client | one OUCH inbound message | yes |
| `H` Server Heartbeat | server | none | yes |
| `R` Client Heartbeat | client | none | yes |
| `O` Logout Request | client | none | yes |
| `Z` End of Session | server | none | handled by the gateway; the simulator never sends it |
| `+` Debug | either | free text | ignored |

Sequenced messages carry no sequence number on the wire. Both sides count them, starting at 1. A
client that reconnects asks for the next number it expects and the server replays from there.
Heartbeats go out after one second of silence in a direction, and a side that hears nothing for
fifteen seconds drops the connection. Both intervals are configurable here so tests can run fast.

## Deliberate simplifications

These are the places where the behaviour is "OUCH-style" by choice rather than a claim of
conformance.

**Messages left out.** Inbound Modify Order (`M`). Outbound AIQ Canceled (`D`), Cancel Pending
(`P`), Cancel Reject (`I`), Order Priority Update (`T`), Order Modified (`M`) and every other
outbound type not listed above, such as broken-trade notices. The gateway journals an
unsupported sequenced message, counts it in `bad_messages`, and still advances the sequence
number.

**Cancel `Shares` is the open quantity to keep.** A cancel reduces the order's open shares to at
most that number, and `0` cancels everything. Sending the same cancel twice has no further
effect. The specification calls this field the new "intended order size", "the maximum number of
shares that can be executed in total after the cancel is applied". This project reads that as the
open quantity that may remain; whether a real venue also counts shares executed before the cancel
is not settled by that sentence, so confirm it before relying on partial cancels.

**Replace `Shares` is the new total size including shares already executed.** The replacement
order's open quantity is `Shares` minus executed shares, as in the specification ("total number
of shares liable, inclusive of previous executions"). If executions already cover the new total,
the simulator cancels the remainder, reports `Canceled` on the original token and leaves the
replacement token unused. A replace always gets a new order reference number and goes to the
back of the queue. Self-match-prevention decrements, which the specification also counts toward
the total, do not exist here.

**Requests for dead or unknown orders are ignored without a response.** A cancel or replace for an
order that is no longer live produces no message, and neither does any request that reuses a token
the account has already used. That second rule is what the gateway's reconnect logic depends on
(see `docs/design.md`), and it follows the specification: "All Inbound Messages may be repeated
benignly", and an Enter Order "with a previously used Order Token ... will be ignored".

**Fields carried but not acted on.** Display, Capacity, Intermarket Sweep Eligibility, Minimum
Quantity, Cross Type and Customer Type are validated loosely, stored and echoed back. They do not
change matching. Time-in-force values other than `0` all mean "rest until canceled": there is no
timed expiry, no market open or close, and no crossing sessions. BBO Weight Indicator is always a
space. Timestamps are nanoseconds since midnight UTC taken from the host clock.

**Market orders.** Price `0x7FFFFFFF` trades against everything available and the unfilled
remainder is canceled with reason `I`. The gateway's risk checks refuse market orders unless
explicitly enabled.

**Matching.** One price-time-priority book per symbol. Trades print at the resting order's price.
No self-match prevention, no hidden or pegged orders, no auctions, no halts.

**Reject reasons the simulator can produce.** `Z` shares out of range, `S` unknown or blank
symbol, `X` invalid price, `D` invalid display value, `N` minimum quantity above order size, `O`
other (blank token, bad side). Cancel reasons: `U` user requested, `I` immediate-or-cancel
remainder.

**Session.** One sequenced stream per account, one live connection per account (a new login
displaces the old connection), and one session id for the simulator's lifetime. A login asking for
a sequence number beyond the end of the stream is rejected with `S`. No TLS, no end-of-day roll.
