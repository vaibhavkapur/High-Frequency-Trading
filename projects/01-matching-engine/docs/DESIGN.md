# Design

## Architecture

```text
 text commands ──encode──┐
                         v
 binary command log ─> me_cli replay ──> decode_command ──> Engine<Book>::process
 (40-byte records)      (streaming)                              │
                                                    ┌────────────┴────────────┐
                                                    v                         v
                                           command validator          per-symbol Book
                                           (reject, no state          (MapBook or PoolBook)
                                            change)                           │
                                                    │                         │ trades
                                                    └──────────┬──────────────┘
                                                               v
                                                     sink(const Report&)
                                                               │
                                     ┌─────────────────────────┼──────────────────────┐
                                     v                         v                      v
                           binary report stream         text reports        tests / benchmarks
                           (64-byte records)                                 (in-memory sinks)

 Engine::top_of_book / Engine::depth ──> snapshots        Engine::stats ──> counters
```

| Component | File | Role |
|---|---|---|
| Types | `include/me/types.hpp` | `Price`, `Qty`, `OrderId`, `SymbolId`, `Seq`, `Side`, `LevelView` |
| Wire protocol | `include/me/protocol.hpp` | `Command` / `Report` records, encode/decode |
| Engine | `include/me/engine.hpp` | validation, sequencing, report construction, snapshots, stats |
| MapBook | `include/me/map_book.hpp` | M1 book: `std::map` levels, `std::list` queues, `std::unordered_map` index |
| PoolBook | `include/me/pool_book.hpp` | M3 book: order pool, intrusive FIFO, sorted level vectors, fixed hash index |
| Text format | `src/text_format.cpp` | human-readable commands and reports |
| Workload | `src/workload.cpp` | deterministic command generator |
| CLI | `src/cli_main.cpp` | `gen`, `encode`, `replay`, `dump` |
| Alloc counter | `src/alloc_counter.cpp` | counting `operator new`, linked only into measuring binaries |
| Reference engine | `tests/reference_engine.hpp` | naive oracle for differential tests |

`Engine` is a class template over the book type, so the book is chosen at compile
time and the trade callback inlines into the matching loop. Both books implement
the same small interface (`add`, `cancel`, `reduce`, `find`, `contains`, `size`,
`best`, `depth`, `check_invariants`).

## Threading model

One engine is a single-writer state machine: `process()` is not thread-safe and
takes no locks. The intended deployment is one engine (or one shard of symbols)
per thread, with network input and report output handed over through queues on
the other side of the `sink` callback. The engine itself does no I/O, logging,
system calls or (with `PoolBook`) heap allocation. Nothing in this repository is
concurrent, which is why there is no ThreadSanitizer configuration.

## Matching rules

* Orders are limit orders that rest until filled or cancelled (good-till-cancel).
* An incoming order matches while its limit crosses the best opposite price:
  best price first, then first-in-first-out within a price level.
* Each fill executes at the **resting** order's price.
* Any unfilled remainder rests at the order's limit price, at the back of that level's queue.
* The engine never leaves a crossed book (`best bid < best ask` is a checked invariant).

### Priority rules for modifications

| Command | Effect on time priority |
|---|---|
| `Reduce` (new remaining quantity lower than current) | **kept**: the order stays where it is in its queue |
| `Replace` (cancel/replace with a new order id) | **lost**, always: the old order is removed and the replacement enters as a new arrival, even when price and quantity are unchanged |

`Reduce` cannot increase quantity or change price, so an order that wants either
must use `Replace` and go to the back of the queue. `Replace` is atomic: if the
replacement fails validation the original order is left untouched. The
replacement keeps the original order's side, may reuse the original id, and may
trade immediately if its price crosses.

## Validation

Every command yields at least one report. A rejected command changes no state
and consumes exactly one sequence number. Checks run in this order (the
reference engine in the tests implements the same order independently):

1. command type is known → else `UnknownCommand`
2. symbol exists → else `UnknownSymbol`
3. `New`: side is Buy/Sell → `InvalidSide`
4. `Cancel` / `Reduce` / `Replace`: the order is resting on that symbol → `UnknownOrderId`
5. `New` / `Replace`: `0 < price <= max_price` → `InvalidPrice`; price is a tick multiple → `InvalidTick`;
   `0 < qty <= max_qty` and a lot multiple → `InvalidQuantity`
6. `Reduce`: `0 < qty < remaining` and a lot multiple → `InvalidQuantity`
7. `New` / `Replace`: the new id is not resting on that symbol → `DuplicateOrderId`
8. `New`: the book holds fewer than `max_orders_per_book` orders → `BookFull`

`BookFull` is decided before matching, so a refused order never trades; the cost
is that a fully marketable order is refused while the book is exactly at its
bound.

## Sequencing and determinism

`Report::seq` is one engine-wide counter starting at 1; every report (ack,
reject, trade, cancel, reduce, replace) takes the next value, across all
symbols. Output depends only on the configuration and the command sequence:
no clocks, no randomness, no addresses, no iteration over unordered containers.
Both record types have no implicit padding (`static_assert`ed) so the report
stream is byte-for-byte reproducible, and `MapBook` and `PoolBook` produce
identical streams.

## Data structures

### MapBook (baseline)

`std::map<Price, Level>` per side, `std::list<Order>` per level,
`std::unordered_map<OrderId, Locator>` holding a list iterator. Simple and
obviously correct, O(log levels) for a new level and O(1) for everything else,
but each resting order allocates a list node and a hash node and each new level
a tree node, and the nodes are scattered across the heap.

### PoolBook (optimized)

* **Order pool.** `std::vector<Order>` sized to `max_orders` at construction;
  orders are addressed by 32-bit index. Free slots form a LIFO list so the most
  recently released (cache-hot) slot is reused first. An order is 40 bytes.
* **Intrusive FIFO.** Each order carries `prev`/`next` indices. A level stores
  `head`, `tail`, `count` and the aggregate quantity. Append, pop-front and
  unlink-from-the-middle are O(1).
* **Levels.** One contiguous `std::vector<Level>` per side, sorted so the best
  price is at the back. Matching reads `back()`; a level that empties at the top
  is a `pop_back()`. Finding a level probes the 8 levels nearest the top
  linearly, then binary-searches the rest. Inserting or erasing a level away
  from the top shifts the levels above it (32 bytes each).
* **Order-id index.** Open-addressing hash table of `{id, pool index}` with
  linear probing, Fibonacci hashing and backward-shift deletion (no
  tombstones). Its capacity is fixed at `bit_ceil(2 * max_orders)` slots, so the
  load factor never exceeds 0.5 and it never rehashes.

| Operation | MapBook | PoolBook |
|---|---|---|
| add to existing level | O(log L) map lookup + 2 allocations | level lookup (≤8 probes, else O(log L)) + O(1), no allocation |
| add creating a level | O(log L) + 3 allocations | lookup + O(distance from top) shift |
| cancel | O(1) expected + O(log L) + 2 frees | O(1) expected + level lookup; O(distance from top) shift if the level empties |
| reduce | O(1) expected + O(log L) | O(1) expected + level lookup |
| match, per fill | O(1) + frees | O(1) |
| top of book | O(1) | O(1) |

L = number of price levels on one side.

## Assumptions and limitations

* **Order ids are unique per symbol, not globally.** Cancel/Reduce/Replace carry the symbol.
  An id can be reused after its order is fully filled or cancelled.
* **Limit orders only.** No market, IOC/FOK, stop, iceberg or pegged orders; no auctions; no
  self-trade prevention; no trading sessions or halts.
* **No participants or risk checks.** Commands carry no account identity.
* **Bounded books.** At most `max_orders_per_book` resting orders per symbol
  (`BookFull` beyond that). `PoolBook` allocates that capacity up front for every
  symbol: 72 to 104 bytes per order of capacity (a 40-byte pool slot plus two to
  four 16-byte index slots, because the index is rounded up to a power of two). Capacity should be sized to
  expected occupancy; over-provisioning measurably hurts (see `docs/BENCHMARKS.md`).
* **Level vectors can grow.** `PoolBook` reserves 4096 levels per side; a book with more
  distinct prices on one side triggers a (counted, amortized) reallocation, and
  insertion far from the top of a very wide book is linear in the number of levels above it.
* **Quantity bound.** `max_qty * max_orders_per_book` must fit in a signed 64-bit integer; the
  engine constructor rejects configurations where a level's aggregate could overflow.
* **Little-endian hosts only** (`static_assert`ed). The wire format is the in-memory layout.
* **No persistence or recovery.** State is rebuilt by replaying the command log.
* **Hash index is not hardened** against adversarially chosen order ids (a fixed multiplicative hash).
* **Snapshots are synchronous** calls on the engine thread; there is no market-data feed.
