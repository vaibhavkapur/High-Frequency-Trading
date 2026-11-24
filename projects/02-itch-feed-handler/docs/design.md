# Design notes

## Pipeline

```text
          file bytes (mmap)                       MoldUDP64 packet
                 |                                       |
     LengthPrefixedReader                 parse_moldudp64 / MoldSequenceTracker
                 \_______________________  _______________/
                                         \/
                              message payload (span of bytes)
                                         |
                    decode()  -- length check, Cursor, big-endian loads
                                         |
                          typed message (AddOrder, OrderExecuted, ...)
                                         |
                     FeedHandler::process  -- routing + trade semantics
                        |                         |
                 BookBuilder<Index>          SymbolStats (volume, signed volume)
          order index -> order pool -> price levels (FIFO per level)
                        |
        ProcessResult { status, timestamp, book_locate, trade }
                        |
      itch_replay: BBO-change CSV, trade CSV, depth snapshot CSV, summary CSV
```

Each layer only knows the one below it: framing never looks inside a payload, the
decoder never touches the book, and the book has no idea where bytes came from.

| Path | Role |
|---|---|
| `include/itch/endian.hpp` | byte-wise big-endian loads/stores |
| `include/itch/messages.hpp` | typed event model, one struct per message |
| `include/itch/decoder.hpp` | checked decoder, `decode()` visitor API, `Message` variant |
| `include/itch/encoder.hpp` | inverse of the decoder; fixtures and synthetic data |
| `include/itch/framing.hpp` | length-prefixed reader, MoldUDP64 parser, gap tracker |
| `include/itch/order_index.hpp` | `FlatOrderIndex` and `StdOrderIndex` |
| `include/itch/book.hpp` | `BookBuilder`, levels, BBO, top-N, digest, derived metrics |
| `include/itch/feed_handler.hpp` | decoder + book + trade statistics, counters |
| `include/itch/replay.hpp` | `Pacer` (unlimited / fixed rate / time-scaled) |
| `include/itch/synth.hpp`, `src/synth.cpp` | deterministic synthetic stream generator |
| `tools/` | `itch_replay`, `itch_gen` |
| `benchmarks/itch_bench.cpp` | staged benchmark harness |
| `scripts/itch_ref.py` | independent Python reference implementation |

## Decoding without struct overlays

The decoder never casts wire bytes to a packed struct. For each message it

1. compares the payload length with the specification length **before** reading
   anything (shorter: `Truncated`, longer: `Oversized`);
2. reads fields in order through a `Cursor` using shift-and-or loads from
   `unsigned char`, which is well defined regardless of host endianness, alignment
   and strict aliasing, and which compilers turn into a load plus byte swap;
3. writes into an ordinary struct with natural alignment.

Two things make "the reader stays inside the buffer" a checked property rather than
a claim:

- `static_assert(wire_size<T>() == T::kLength)` for every message type. The readers
  are `constexpr`, so the compiler runs each one over a buffer and proves it consumes
  exactly the specification length. A reader that drifted from its declared length
  would not compile.
- Tests decode every truncation of every message type from an exact-size heap block
  under AddressSanitizer.

## Book representation

- **Order pool.** Orders live in one `std::vector<Node>` addressed by 32-bit slot,
  with an intrusive free list, so steady-state adds and removes allocate nothing.
- **Order index.** Maps the 64-bit order reference to a pool slot. It is a template
  parameter so two implementations can be compared (see `docs/benchmarks.md`):
  `FlatOrderIndex` (open addressing, linear probing, Fibonacci hashing, backward-shift
  deletion, 16-byte slots) and `StdOrderIndex` (`std::unordered_map`).
- **Price levels.** Per (stock locate, side), a `std::vector<Level>` sorted worst to
  best, so the inside of the book is at the back. Lookup is a binary search; inserts
  and erases near the touch move few elements.
- **Time priority.** Each level holds a doubly linked FIFO of its orders (links are
  pool slots). ITCH does not require this to compute depth, but it is what makes
  "replace loses priority" an observable, tested property, and it is the structure a
  queue-position model would need.
- **Books by locate.** `std::vector<OrderBook>` indexed by stock locate, grown on
  demand; the specification intends locate codes to be used as array indexes.

`BookBuilder::digest()` hashes every symbol, level and resting order in queue order.
Two runs agree on the digest only if they agree on all prices, sizes and priorities;
the determinism tests and the benchmark harness both rely on it.

## Error policies

Nothing in the pipeline throws or aborts on bad input. Every case has a counter,
printed by `itch_replay`.

| Layer | Condition | Policy | Counter |
|---|---|---|---|
| framing | zero-length frame | skip the 2-byte prefix, continue | `zero-length frames` |
| framing | 1 stray byte / payload runs past end | stop; everything before it was processed; `itch_replay` exits 3 | `framing end=` |
| decode | empty payload | reject | `empty` |
| decode | type byte not in the matrix | reject | `unknown_type`, `seen_by_type` |
| decode | shorter than spec length | reject | `truncated` |
| decode | longer than spec length | reject (strict: a longer message means an unknown revision) | `oversized` |
| decode | side not `B`/`S` in `A`/`F`/`P` | reject | `invalid_field` |
| book | add with a live order reference | reject the add | `duplicate_order` |
| book | execute/cancel/delete/replace of unknown reference | ignore | `unknown_order` |
| book | execute/cancel more than remains | clamp to remaining, remove order | `over_reduce` |
| book | replace onto a live new reference | original removed, new add rejected | `duplicate_order` |
| feed | header locate differs from the order's locate | apply using the order's locate | `locate_mismatch` |

A length-prefixed stream has no resynchronisation marker, so a corrupted length
cannot be recovered from; the reader stops rather than guess.

## Determinism

Replay is single threaded and processes messages strictly in file order. There is no
wall-clock input, no hashing with per-process seeds in any output path, and no
iteration over unordered containers when producing output. Pacing changes when a
message is released, never the order. The test suite replays each profile twice and
with both index implementations and requires identical digests, trade counts and
book-change counts.

## Assumptions and limitations

- **File replay only.** No sockets. MoldUDP64 is parsed from buffers and gaps are
  detected, but nothing requests retransmission. This is not evidence of live
  exchange connectivity.
- **Not validated on real Nasdaq data in this repository.** Layouts were checked
  against the specification and the pipeline was cross-checked against an independent
  implementation on synthetic and corrupted streams, but no licensed Nasdaq sample
  was available. `scripts/crosscheck.py --file` exists for running that check on a
  real file.
- **One trading day per process.** Order reference numbers are day-unique; state is
  never reset between sessions.
- **Trading state is not modelled.** Halts, pauses and auction periods are decoded
  but do not change how the book is built or how BBO is reported; a crossed or locked
  book during an auction is reported as-is.
- **Broken trades do not unwind statistics.**
- **Input files must be uncompressed.** Nasdaq distributes `.gz`; decompress first.
- **Output is CSV.** Parquet is not implemented.
- **32-bit order slots** cap the number of simultaneously live orders at 2^32 - 1.
- **POSIX only** (`mmap`); built and tested on macOS/arm64. The CI configuration
  targets Linux and macOS but has not been run (the directory is not yet a git
  repository).
