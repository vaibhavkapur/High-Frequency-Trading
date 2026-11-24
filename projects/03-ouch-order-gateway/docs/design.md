# Design notes

## Components

```text
                    gateway process (one thread, poll-driven)
  +-----------------------------------------------------------------------+
  |  caller: submit / cancel / replace                                    |
  |      |                                                                |
  |      v                                                                |
  |  RiskEngine --refused--> counted and journaled, never sent            |
  |      | ok                                                             |
  |      v                                                                |
  |  OrderManager.on_*_sent   (order becomes PendingNew / pending cancel) |
  |      |                                                                |
  |      v                                                                |
  |  EventLog.append          (write-ahead: journal first)                |
  |      |                                                                |
  |      v                                                                |
  |  wire::encode -> soup::append_packet -> net::Conn  ===== TCP ====>    |
  |                                                                       |
  |  OrderManager.on_exchange <- EventLog.append <- wire::decode          |
  |      ^                                         <- soup::FrameParser <=|== TCP
  |      |                                                                |
  |  listener callback to the caller                                      |
  +-----------------------------------------------------------------------+

                    exchange simulator (own thread)
  +-----------------------------------------------------------------------+
  |  SoupBinTCP server -> fault injector -> OUCH handler -> MatchingEngine |
  |  per-account sequenced log  <- Accepted / Executed / Canceled / ...   |
  |  delivery cursor per connection (replay = move the cursor back)       |
  +-----------------------------------------------------------------------+
```

| Component | Files | Knows about |
|---|---|---|
| Wire codec | `wire.hpp`, `soupbin.hpp` | Bytes only. No sockets, no order state. |
| Network | `socket.hpp` | File descriptors and a send buffer. No message formats. |
| Order state machine | `order_manager.hpp` | Decoded messages. No I/O, no clock. |
| Risk | `risk.hpp` | Limits and exposure numbers. |
| Journal | `event_log.hpp` | Records, and how to replay them into an `OrderManager`. |
| Gateway | `gateway.hpp` | Wires the above together and owns the session. |
| Simulator | `sim/` | Its own book, sessions and fault injection. Shares only the codec and socket helpers. |

The order manager takes time as an argument and does no I/O, so every test of it is deterministic
and its whole state is a function of the events applied to it.

## Order state machine

```text
                 +------------- Rejected <-------------+
                 |                                     |
  submit -> PendingNew --disconnect--> Reconciling ----+
                 |                          |
                 | Accepted                 | Accepted / Executed / Canceled (via replay or resend)
                 v                          v
               Live  <----------------------+
                 |  \
        Executed |   \ Canceled (leaves 0)
                 v    \
        PartiallyFilled -> Canceled
                 |
        Executed | (leaves 0)        Replaced message naming this order as "previous"
                 v                              |
               Filled                           v
                                             Replaced
```

`Filled`, `Canceled`, `Rejected` and `Replaced` are terminal. A cancel or replace in flight is
tracked separately from the state (`pending`), with a flag (`pending_uncertain`) set when the
session dropped before the answer came back.

Quantities: `qty = cum_qty + leaves` for any working order. `leaves` is the only number that
feeds exposure, and it changes in exactly one function (`set_leaves`), which also updates the
per-symbol totals. `check_invariants()` recomputes everything from scratch and the tests call it
after every scenario, including after every step of a random message storm.

### Idempotent application

The exchange is the source of truth. Every exchange message is applied so that a second copy
changes nothing.

| Message | Recognised as a duplicate when |
|---|---|
| Accepted | The order is no longer awaiting acknowledgement. |
| Rejected | The order is already terminal. |
| Executed | This order has already recorded this match number. |
| Replaced | The replacement order already has a reference number. |
| Canceled | The order is already terminal, or (reason `U`) every cancel request sent for the order has already been answered and no replace is pending. |

The Canceled rule is contextual because the message has no unique id. It has a known gap: if a
second cancel request for the same order is already in flight when a duplicate of the first
partial cancel arrives, the duplicate is taken as the answer to the second request. A cancel that
takes the order to zero is unaffected, since a terminal order ignores further cancels. Exchange
initiated partial reductions (reason other than `U`) cannot be deduplicated at all. The fault
storm test sends at most one cancel per order for this reason.

### Out-of-order delivery

SoupBinTCP over one TCP connection cannot reorder, so this only happens when the simulator is told
to do it. The state machine still copes, because an execution or cancel for an order proves the
exchange accepted it:

| Arrives first | Then | Handling |
|---|---|---|
| Executed | Accepted | Implicit accept; the late Accepted only supplies the reference number. |
| Canceled | Accepted | Implicit accept, then canceled; late Accepted ignored. |
| Executed or Canceled on replacement | Replaced | Replacement implicitly accepted at an estimated size. The late Replaced retires the original and corrects the open quantity by the estimation error only, so fills and cancels already applied are not undone. |
| Canceled (partial) | Executed | Both applied; final quantities match, final state may read `Filled` where the exchange would say "canceled after partial fill". |

### Cancels the exchange never answers

The exchange is silent about a cancel that removes nothing. Two cases would otherwise leave a
cancel pending forever, and both are handled in one place: whenever an acknowledged order's open
quantity is at or below the pending cancel's target, the cancel is treated as done.

- The order filled, fully or down to the target, before the cancel arrived.
- The cancel was sent against a replacement that had not been confirmed yet, using its estimated
  size, and the exchange then reported a smaller real size.

This is safe because the gateway's open figure is never lower than the exchange's. If the
exchange did act on the cancel first, its Canceled message is still applied when it arrives.

### Replace

A replace creates the replacement order record immediately (state `PendingNew`, linked to the
original). Until the exchange answers, both orders count as exposure. Outcomes:

- `Replaced`: original becomes `Replaced`, replacement becomes `Live` with the exchange's
  outstanding share count.
- `Rejected` for the replacement token: original is untouched.
- The original reaches a terminal state first. The exchange will ignore the replace and send
  nothing, so the order manager terminates the replacement itself with reason `#`. If a `Replaced`
  for it does show up later, the exchange wins and the replacement is revived.

## Reconnect and reconciliation

The problem: the connection drops after a request was written and before its response was read.
The request may have been lost, or accepted, or accepted and filled.

What the gateway does:

1. **Marks, does not guess.** On disconnect every unanswered new order moves to `Reconciling`
   and every unanswered cancel or replace is flagged uncertain. Uncertain orders keep counting
   as exposure, since they may be live. Acknowledged orders are not in doubt and keep their state.
2. **Refuses new work while down.** `submit`, `cancel` and `replace` return `NotConnected`.
   Nothing is queued for later, so no stale order can be released after an outage.
3. **Resumes the sequenced stream.** The login request carries the next sequence number the
   gateway expects and the session id it was using. The exchange replays everything from that
   point, which delivers any responses that were produced but never read.
4. **Resolves what the replay did not.** Under the default policy the gateway re-sends each
   uncertain request with its **original token**. The exchange ignores a token it has already
   seen, so the request takes effect at most once: if the first copy arrived, the resend is
   dropped and the replay supplies the answer; if it did not, the resend is the first copy.
   Cancels are idempotent by construction ("reduce open shares to N"). A replace is re-sent with
   its original replacement token and is ignored if that token was already used. The OUCH 4.2
   specification describes exactly this recovery: "All Inbound Messages may be repeated
   benignly", so that a client "can safely re-send any pending messages ... without worrying
   about generating duplicates".

The order stays in `Reconciling` until an actual response arrives. Re-sending proves nothing by
itself.

Why not wait for the replay to finish and only then decide? SoupBinTCP does not mark the end of a
replay, so the gateway cannot tell "the Accepted is still coming" from "the order never arrived".
Because the resend is harmless in both cases, it does not need to know.

What this does not give you: exactly-once execution. It gives at-most-once *effect* per token,
provided the exchange deduplicates tokens. That is a property of the exchange, verified here only
against the simulator.

The other policy, `ReconcilePolicy::Hold`, sends nothing automatically. Uncertain orders stay in
`Reconciling` until the replay resolves them or the owner calls `resend_uncertain()` or `cancel()`.
Use it when re-entering an order after an outage is not acceptable without a human or strategy
decision. Under the default policy an order is re-sent at its original price however long the
outage lasted; there is no staleness check.

## Journal

Append-only file, one record per state-changing event: request sent, request re-sent, exchange
message received (with its sequence number), session up, session down, risk reject. Requests are
journaled before they are written to the socket.

On construction the gateway replays an existing journal to rebuild order state, the next expected
sequence number, the session id and the token counter, then treats everything unanswered as
uncertain. `ReplayRebuildsIdenticalOrderState` and `RestartRecoversStateFromJournalAndReconciles`
cover this. A torn final record is ignored.

Durability is what `fflush` gives: with `journal_flush_each` every record reaches the OS before
the send; without it records sit in a user-space buffer and a crash can lose the tail. There is
no `fsync`, so neither mode survives power loss. Losing the tail is safe for exchange messages
(they are re-read by replay) and unsafe for requests: a request that reached the exchange but not
the journal is an order the restarted gateway does not know about. `flush` mode closes that window
against process crashes only.

## Pre-trade risk

Checked on every `submit` and `replace`, in this order: zero quantity, maximum order quantity,
market orders allowed, reference price present, maximum notional, price band around the reference
price, position limit. The position check is worst-case: current position plus all open and
unacknowledged quantity on that side plus the new order. A replace is checked against the
symbol's exposure without the order it replaces, because the exchange swaps the two atomically.

Reference prices are supplied by the caller (`set_reference_price`). There is no market data feed
in this project.

## Fault injection

All faults are injected by the simulator, never by the network.

| Fault | Mechanism | What it exercises |
|---|---|---|
| Lost request + disconnect | Discard the inbound message, close the connection | `Reconciling`, same-token resend |
| Processed request + disconnect | Process it, withhold responses, close | Sequenced replay, exchange-side token dedup |
| Duplicate response | Append the message to the sequenced log twice | Idempotent application |
| Out-of-order response | Hold a message until after the next one for that account | Implicit accepts |
| Delayed response | Give the log entry a future release time | Pending-state exposure, timers |
| Silent server | Stop sending data and heartbeats | Heartbeat timeout |

Scripted faults target one specific request, for deterministic tests. Random faults draw from a
seeded generator, for the storm tests and the benchmark. In both cases the faulty stream is what
the simulator records as its sequenced log, so a replay reproduces the same duplicates and
ordering.

## Threading

The gateway is single-threaded. `poll()` does all socket I/O, timers and reconnects, and listener
callbacks run inside it. The initial TCP connect is blocking. The simulator runs one event-loop
thread guarded by one mutex, which its control and observation methods also take. Nothing is
lock-free and nothing is pinned to a core.

## Limitations

- Not a certified or tested integration with any real venue. See `docs/protocol.md`.
- IPv4 loopback addresses only; the simulator binds 127.0.0.1.
- One account per gateway, one symbol universe, no market data, no strategy.
- The matching core is a compact stand-in. `sim::MatchingEngine` is the interface where the
  separate matching-engine project would plug in; that project did not exist when this was written.
- Order records are kept for the life of the process; nothing is evicted.
- Token counter recovery assumes the journal is complete (see the durability note above).
- Developed and measured on macOS/arm64. The code uses only POSIX sockets and `poll`, and CI is
  set up for Linux, but the Linux build had not been run when this was written.
