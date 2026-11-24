# Protocol reference and supported-message matrix

## Specification

- **Document:** *Nasdaq TotalView-ITCH 5.0*, published by Nasdaq.
- **URL:** <https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf>
- **Revision implemented:** the copy retrieved on 2026-11-24, whose latest revision-log
  entry is **April 28, 2023** (adds the Direct Listing with Capital Raise message and
  the "Paused" NOII imbalance direction).

Every offset and length in `include/itch/messages.hpp` / `decoder.hpp` was checked
field by field against the tables in that document. The specification itself is not
redistributed in this repository.

ITCH is an outbound, public market-data protocol. Nothing here sends orders or
connects to an exchange.

## Data types

| Spec type | Wire | Decoded as |
|---|---|---|
| Integer | big-endian unsigned, 1/2/4/6/8 bytes | `uint8/16/32/64_t` |
| Timestamp | 6-byte integer, nanoseconds since midnight | `Timestamp` (`uint64_t`) |
| Alpha | ASCII, left-justified, space-padded | `char`, `std::array<char, N>`, `Symbol` |
| Price (4) | 4-byte integer, 4 implied decimals | `Price4` (`uint32_t`), never converted to floating point in the pipeline |
| Price (8) | 8-byte integer, 8 implied decimals | `Price8` (`uint64_t`) |

Every message starts with the same 11 bytes: type (1), stock locate (2), tracking
number (2), timestamp (6).

## Supported-message matrix

All 23 message types in the specification are decoded into typed structs. "Book"
says what the book builder does with the message.

| Type | Message | Length | Decoded | Book / statistics effect |
|---|---|---:|:---:|---|
| `S` | System Event | 12 | yes | none |
| `R` | Stock Directory | 39 | yes | sets the symbol name for the stock locate |
| `H` | Stock Trading Action | 25 | yes | none (halts do not clear the book) |
| `Y` | Reg SHO Restriction | 20 | yes | none |
| `L` | Market Participant Position | 26 | yes | none |
| `V` | MWCB Decline Level | 35 | yes | none |
| `W` | MWCB Status | 12 | yes | none |
| `K` | IPO Quoting Period Update | 28 | yes | none |
| `J` | LULD Auction Collar | 35 | yes | none |
| `h` | Operational Halt | 21 | yes | none |
| `A` | Add Order (no MPID) | 36 | yes | **add** order to book |
| `F` | Add Order (MPID attribution) | 40 | yes | **add** order to book (attribution decoded, not stored) |
| `E` | Order Executed | 31 | yes | **reduce** order; trade at the resting price |
| `C` | Order Executed With Price | 36 | yes | **reduce** order; trade at the message price; counted in volume only if printable |
| `X` | Order Cancel | 23 | yes | **reduce** order |
| `D` | Order Delete | 19 | yes | **remove** order |
| `U` | Order Replace | 35 | yes | **remove** original, **add** new reference at the back of its level |
| `P` | Trade (non-cross) | 44 | yes | trade report only; **book untouched** |
| `Q` | Cross Trade | 40 | yes | trade report only; **book untouched** |
| `B` | Broken Trade | 19 | yes | counted; statistics are not unwound |
| `I` | NOII | 50 | yes | none |
| `N` | RPII | 20 | yes | none |
| `O` | DLCR Price Discovery | 48 | yes | none |

Lengths include the type byte and exclude transport framing.

## Semantics that are easy to get wrong

These follow the specification text; the section numbers are the spec's.

- **Order reference numbers are day-unique across the feed** (1.3), not per symbol.
  The order index is therefore feed-wide and each order remembers its stock locate.
  `E`/`C`/`X`/`D`/`U` are applied by order reference alone; the header's stock locate
  is only compared against the order's locate (`locate_mismatch` counter).
- **Replace** (1.4.5) retires the original reference and introduces a new one "which
  will be used henceforth"; side, symbol and attribution carry over from the original
  add. The new order joins the back of its price level. Any later message using the
  old reference is an unknown-order event.
- **Executed shares are cumulative deductions** (1.4); an order whose displayed size
  reaches zero leaves the book.
- **`C` with Printable = `N`** (1.4.2) is part of a later bulk print. It changes the
  book but is excluded from trade count and volume to avoid double counting with the
  `Q` cross print.
- **`P` trades do not affect the book** (1.5.1). Since 2014-07-14 their Buy/Sell
  indicator is always `B` regardless of the resting side, so `P` trades are reported
  without an aggressor side and are not included in signed volume.
- **`Q` cross trades** (1.5.2) report bulk cross volume, tracked separately as
  `cross_volume`. The orders that took part are removed by their own messages.
- **`B` broken trades** (1.5.3) "have no impact on the current book". They are
  counted; previously accumulated volume is not adjusted (a limitation).
- **Aggressor side** for `E`/`C` is inferred as the opposite of the resting order's
  side. This is an inference from the feed, not a field in it.

## Transport framing

Message bodies never carry their own length; a lower layer does. Two framings are
implemented in `include/itch/framing.hpp`, both as buffer parsers:

| Framing | Layout | Status |
|---|---|---|
| Length-prefixed file | 2-byte big-endian length, then the message | used by `itch_replay`, tests, benchmarks |
| MoldUDP64 downstream packet | session (10), sequence (8), count (2), then `count` blocks of 2-byte length + message; count `0` = heartbeat, `0xFFFF` = end of session | parser and sequence/gap tracker, unit tested on hand-built packets; **no sockets, no retransmission requests** |

SoupBinTCP is not implemented.
