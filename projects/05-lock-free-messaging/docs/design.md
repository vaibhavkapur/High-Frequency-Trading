# Design notes

This document is the reasoning behind the code: what is supported, why the
atomics use the orderings they do, what progress guarantees are (and are not)
claimed, and what each test is evidence for.

## 1. Supported subset

| Component | Header | What it is |
|---|---|---|
| `SpscQueue<T>` | `lfm/spsc_queue.hpp` | Bounded single-producer/single-consumer ring. The main deliverable. |
| `MpscQueue<T>` | `lfm/mpsc_queue.hpp` | Separate bounded multi-producer/single-consumer queue (experimental). |
| `MutexQueue<T>` | `lfm/mutex_queue.hpp` | Mutex + condition-variable reference queue for tests and benchmarks. |
| `Producer` / `Consumer` | `lfm/endpoints.hpp` | Overload policy (backpressure or drop), counters, wait policies. |
| wait policies | `lfm/wait_policy.hpp` | `BusySpin`, `YieldWait`, `SpinThenYield`. |
| affinity | `lfm/affinity.hpp` | Hard pinning on Linux; QoS core-class hint on macOS. |

Not provided: MPMC, unbounded queues, blocking (futex/condvar) waits on the
lock-free queues, inter-process shared-memory transport, variable-length
messages.

## 2. SPSC layout

```text
              producer thread                          consumer thread
                    |                                        ^
          construct | slot[tail & mask]      move out of     | slot[head & mask]
                    v                                        |
        +-------+-------+-------+-------+-------+-------+-------+-------+
 slots  |       |  h    |  x    |  x    |  x    |  t    |       |       |   power-of-two ring,
        +-------+-------+-------+-------+-------+-------+-------+-------+   separate allocation

 object | capacity, mask, slots* | tail (atomic), head_cache | head (atomic), tail_cache |
        |   read-only, shared    |      producer-owned       |      consumer-owned       |
        '------ cache line ------'------- cache line --------'------- cache line --------'

   producer:  tail.store(release)  ------------------------>  consumer: tail.load(acquire)
   producer:  head.load(acquire)   <------------------------  consumer: head.store(release)
```

* `head` and `tail` are free-running 64-bit counters; the slot index is
  `counter & (capacity - 1)`. Because the counters never wrap in practice
  (2^64 messages), `tail - head` is the exact occupancy and **all `capacity`
  slots are usable** — there is no sacrificial empty slot.
* Queue is empty iff `head == tail`, full iff `tail - head == capacity`.
* Each side keeps a private, possibly stale copy of the other side's cursor
  (`head_cache`, `tail_cache`) and only re-reads the shared atomic when the
  stale copy says "full" (producer) or "empty" (consumer). On the common path
  a push or pop touches no cache line owned by the other thread except the
  slot itself.

## 3. Memory-ordering rationale (M1)

Notation: P = producer thread, C = consumer thread.

**Single-writer cursors.** Only P writes `tail`; only C writes `head`. So each
thread may read *its own* cursor with `memory_order_relaxed`: it is reading a
value it wrote itself, and no other thread can have changed it.

**The one ordering requirement.** For message number `n` (stored in slot
`n & mask`):

1. P constructs the object in the slot (plain, non-atomic writes).
2. P executes `tail.store(n + 1, release)`.
3. C executes `tail.load(acquire)` and sees a value `>= n + 1`.
4. C reads the slot (plain, non-atomic reads).

By [atomics.order], a release store *synchronizes with* an acquire load that
reads the stored value (or a later value in the same release sequence, which is
the case here because every store to `tail` is a release store by the same
thread). Sequenced-before on each thread plus that synchronizes-with edge gives
`(1) happens-before (4)`. So C's read of the slot is not a data race and sees
the fully constructed object. Without release/acquire (e.g. relaxed on both),
there would be no happens-before edge, the slot accesses would be a data race —
undefined behaviour — and on weakly ordered hardware (ARM, POWER) C could
really observe the new `tail` before the slot contents.

The symmetric argument covers slot reuse. C moves the object out and destroys
it, then `head.store(n + 1, release)`. P may only write slot `n & mask` again
for message `n + capacity`, and before doing so it must have observed
`head >= n + 1` through `head.load(acquire)` (that is the only way the "full"
check can pass for that position). Hence C's destruction happens-before P's
re-construction: no overlap, no race.

**Why stale caches are safe.** `head_cache <= head` always (C only increments
`head`). P computes free space as `capacity - (tail - head_cache)`, which can
only *under*-estimate the truth. A stale cache can therefore cause a spurious
refresh, never an overwrite. The same holds for C with `tail_cache <= tail`:
it can only under-estimate how many messages are available. When the stale
value says "full"/"empty" the thread reloads once with `acquire` and decides on
the fresh value, so `try_push` returns `false` only if the queue really was
full at the moment of that load (and likewise for `try_pop` and empty).

**Why not `seq_cst`.** Nothing here needs a single total order over operations
on *different* atomics (no Dekker-style "I write mine then read yours"
protocol). `seq_cst` stores would add a full barrier on x86 (`xchg`/`mfence`)
for no additional guarantee the algorithm uses.

**`volatile` is not used** and would not help: it provides no atomicity and no
inter-thread ordering.

**`size_approx()`** reads `head` then `tail` from any thread. Since `tail` is
monotonic and `head <= tail`, the difference is non-negative, but it mixes two
instants and can momentarily exceed `capacity` for an outside observer; it is
clamped and documented as approximate.

## 4. Progress guarantees — stated narrowly

* `SpscQueue::try_push`, `try_pop`, `try_emplace`: **wait-free**, in the
  specific sense that each call executes a bounded number of its own steps
  regardless of what the other thread does. The proof is by inspection: there
  is no loop and no retry — at most two atomic loads, one copy/move of `T`,
  one atomic store. (This assumes `T`'s copy/move itself is bounded, and that
  `std::atomic<uint64_t>` is lock-free, which is `static_assert`ed.)
* The batch operations are likewise bounded by the batch size.
* `Producer::push` / `Consumer::pop` (the waiting helpers) are **not**
  wait-free or lock-free: they loop until the peer makes room/data. They never
  block in the kernel, but "does not block" is not the same as "makes
  progress".
* `MpscQueue` is **not lock-free as a whole** — see §7.
* A busy-spinning consumer proves nothing about lock-freedom; the guarantees
  above come from the structure of the code, not from the absence of a mutex.

## 5. Element storage and lifetime

* Slots are raw aligned storage (`operator new` with `align_val_t`). Objects
  are created with `std::construct_at` on push and destroyed with
  `std::destroy_at` on pop, so `T` does not need a default constructor and no
  constructor runs for empty slots.
* Works for trivially copyable messages (the intended use; batch copies
  compile down to `memcpy`) and for move-only/non-trivial types
  (`std::unique_ptr`, `std::string` are exercised in tests).
* The destructor destroys any elements still queued. It must not race with
  producers or consumers — destroying a queue in use is a caller bug.
* Exception safety: if `T`'s constructor throws in `try_emplace`, `tail` has
  not been published and the queue is unchanged. The batch APIs require
  nothrow copy/move so a batch can never be half-published. `consume(f)`
  commits progress with a scope guard if `f` throws.
* No allocation after construction: `try_*` never call `new`.

## 6. Cache-line layout and false sharing

The producer-owned line (`tail`, `head_cache`) and the consumer-owned line
(`head`, `tail_cache`) are `alignas(kCacheLine)`; the read-only fields get
their own line so that neither side's writes invalidate them. `kCacheLine` is
128 on Apple Silicon and 64 elsewhere (`-DLFM_CACHE_LINE_SIZE=` overrides).
The slot array is a separate cache-line-aligned allocation.

`SpscQueue<T, 8>` packs all three groups into one line. It exists only so the
benchmark can measure what the padding buys (`spsc` vs `spsc_unpadded`).

What padding cannot remove: the *true* sharing on the slot array and on the
cursors when the queue runs empty or full — then each side must re-read the
other's cursor on every operation, and the line ping-pongs regardless.

## 7. MPSC queue (M5)

A separate implementation (Vyukov-style per-slot sequence numbers, single
consumer), not a generalisation of the SPSC code.

### Rules

Each slot has an atomic `seq`, initially its index. For queue position `pos`
(slot `pos & mask`):

| `seq` value | Meaning |
|---|---|
| `pos` | free; may be reserved for position `pos` |
| `pos + 1` | holds the published value for position `pos` |
| `pos + capacity` | consumed; free for position `pos + capacity` |

* **Reserve**: a producer that reads `seq == pos` attempts
  `CAS(tail: pos -> pos + 1)`. Only the winner may touch the slot.
* **Publish**: the winner constructs the value, then `seq.store(pos + 1, release)`.
* **Consume**: the consumer reads slot `head` only if `seq.load(acquire) ==
  head + 1`; it moves the value out and stores `seq = head + capacity`
  (release), handing the slot back.

The data hand-off is ordered entirely by the `seq` release/acquire pairs (the
same argument as §3, per slot). The CAS on `tail` only arbitrates between
producers and carries no data dependency, so it is `relaxed`.

### Invariants

* **I1** A producer never writes a slot holding an unread value.
* **I2** The consumer never reads an unpublished slot.
* **I3** Values from one producer are consumed in that producer's push order
  (implies no duplication). Across producers the order is CAS order.
* **I4** No loss, deadlock or livelock: every reachable state can reach the
  state where everything pushed has been consumed.

`capacity >= 2` is required: with capacity 1, `pos + 1` ("published") equals
`pos + capacity` ("free for the next lap"), the states are indistinguishable
and I1 breaks. The constructor rejects it. (This was found by the stress test
during development, and the model checker reproduces it.)

### Evidence

`tests/mpsc_model_check.cpp` transcribes the algorithm into a state machine
with one shared access per step and exhaustively explores all interleavings
for up to 3 producers (hundreds of thousands of states), checking I1–I4,
including I4's liveness half via backward reachability. Two deliberately broken
set-ups (a non-atomic CAS; capacity 1) must — and do — produce violations,
which shows the checker is able to fail.

Limits of that evidence: the model is sequentially consistent (it does not
explore weak-memory reorderings; §3-style reasoning and TSan cover that
separately), it is a hand transcription that could diverge from the C++, and
it covers only small bounded configurations. It is model-based testing, not a
formal proof.

### Progress and semantics caveats

* Producers are lock-free relative to each other (a failed CAS means another
  producer succeeded).
* A producer preempted between reserve and publish stalls the consumer at that
  position even if later positions are published (head-of-line blocking), and
  eventually stalls other producers when the ring fills. So the queue is
  **not lock-free** overall, and not wait-free.
* `try_pop` returning `false` means "empty **or** head not yet published".
* `try_push` can report full spuriously while the consumer is mid-pop.
* The constructor of `T` must be `noexcept`: throwing between reserve and
  publish would jam the queue. This is enforced with a `requires` clause.
* Adjacent slots' `seq` fields share cache lines, so producers contend on slot
  lines as well as on `tail`. Padding slots would trade memory for that.

## 8. Overload and wait policies

`try_push` returning `false` is the primitive. `Producer` layers a policy on it:

* `push_or_drop` — never waits; counts `dropped`. Newest message is the one
  discarded; queued data is never overwritten.
* `push` — backpressure: waits per the wait policy; counts `full_events`
  (episodes) and `wait_spins` (iterations).

Counters are single-writer relaxed atomics (plain load + store, no locked
instruction) so another thread can read them without a data race; readers may
see slightly stale values.

| Policy | Wake-up latency | CPU while idle | Notes |
|---|---|---|---|
| `BusySpin` | lowest | 100% of a core | needs a dedicated core per thread |
| `SpinThenYield` | low under load | high, but shares the core | default |
| `YieldWait` | scheduler-dependent | shares the core | fine for tests/oversubscription |

No policy sleeps in the kernel; a parked/futex wait is out of scope.

## 9. Thread placement

* Linux: `pin_current_thread(cpu)` uses `pthread_setaffinity_np`. Benchmarks
  take `--producer-cpu/--consumer-cpu` so same-core-complex, cross-core and
  cross-socket layouts can be compared.
* macOS on Apple Silicon has no hard-affinity API. `hint_core_class` sets the
  thread QoS class, which biases the scheduler towards performance or
  efficiency cores but does not pin. Benchmarks on macOS therefore cannot
  control which two cores are used, cannot prevent migration, and say so in
  their output.

## 10. Assumptions and limitations

* Exactly one producer thread and one consumer thread per `SpscQueue` at any
  time (hand-over between threads is fine if externally synchronised). This is
  not checked at runtime; violating it is a data race.
* Capacity is fixed at construction and must be a power of two.
* 64-bit lock-free atomics are required (`static_assert`).
* In-process only; no persistence, no cross-process sharing.
* The lock-free queues never sleep: an idle consumer burns CPU or yields.
* Benchmarks in `results/` come from a laptop without core isolation or hard
  pinning; see `docs/benchmarking.md` before quoting any number.

## 11. What each test demonstrates

| Requirement | Test |
|---|---|
| In-order, monotonically numbered delivery, one producer/one consumer | `spsc_stress_test` (`single_item`, `batched`) |
| Wraparound over many multiples of capacity | `spsc_basic_test::wraparound_preserves_order`; stress at capacity 1, 2, 16 |
| Full/empty outcomes; unread data never overwritten | `spsc_basic_test::full_push_fails_without_overwrite`, `capacity_one`; `fuzz_ops` |
| Same message mix vs a reference queue | `spsc_stress_test::matches_reference_queue`; `fuzz_ops` (vs `std::deque` model and `MutexQueue`) |
| Object lifetime balanced, non-trivial and move-only types | `spsc_basic_test::lifetime_is_balanced`, `move_only_and_non_trivial_types`; `fuzz_ops` (`Tracked`) |
| Property/fuzz testing of operation sequences | `fuzz_ops` (standalone deterministic driver; libFuzzer target with `-DLFM_LIBFUZZER=ON`) |
| Drop/backpressure counters, wait policies | `endpoints_test` |
| MPSC: no loss/dup, per-producer FIFO | `mpsc_test`; `mpsc_model_check` |
| Data races, memory errors, UB | whole suite under TSan and ASan+UBSan (`scripts/run_sanitizers.sh`) |

Sanitizer caveat: ThreadSanitizer only observes interleavings that occurred in
that run and reasons with its own happens-before model. A clean run is evidence
that the tested executions are race-free; it does not prove the ordering
argument in §3, and it does not simulate weak-memory behaviours.
