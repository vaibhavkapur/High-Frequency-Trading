# Design & Architecture Notes

## 1. System Architecture

```text
+---------------------+       +--------------------------+       IPv4 UDP Multicast (MDF1)
| ExchangeSimulator   |------>| MulticastFeedPublisher   |====================================+
| (deterministic RNG) |       | - Authoritative book     |                                    |
+---------------------+       | - Monotonic seq_num      |                                    v
                              | - Optional FaultInjector |                       +-------------------------+
                              +------------+-------------+                       | Multicast Group         |
                                           | record_packet() +                   | 239.255.x.x:port        |
                                           | replay checkpoints                  +----+---------------+----+
                                           v                                          |               |
                              +--------------------------+                            v               v
                              | RecoveryStore            |                   +--------------+  +--------------+
                              | - Bounded retx ring buf  |                   | Subscriber A |  | Subscriber B |
                              | - Consistent checkpoints |                   | (Reference)  |  | (+FaultInj)  |
                              +------------+-------------+                   +------+-------+  +------+-------+
                                           ^                                        |                 |
                                           |                                        +--------+--------+
                              +------------+-------------+                                   |
                              | TcpRecoveryServer (MDR1) |<==== TCP Gap / Snapshot Reqs =====+
                              +--------------------------+
```

## 2. Receiver State Machine & Invariants

Each feed channel runs an independent `ChannelReceiver` state machine (`LIVE` → `GAP_WAIT` → `SYNCING_SNAPSHOT` → `LIVE`):

1. **No silent book corruption**: When packet `seq > expected_seq` arrives, the receiver **never** applies its events out of order. It raises a `GapAlarm`, transitions to `GAP_WAIT`, and holds future packets in a bounded reorder map (`max_reorder_buffer`).
2. **Bounded reordering window before TCP round-trip**: Naturally reordered UDP datagrams arriving within `reorder_window_packets` or `reorder_timeout_us` fill the hole locally with zero TCP traffic.
3. **Explicit duplicate & corruption visibility**:
   - `seq < expected_seq` or already buffered → dropped and counted in `duplicate_packets`.
   - Bad CRC32 / truncated / invalid header → rejected immediately and counted in `checksum_errors` or `malformed_packets`.
4. **Retransmit vs. Snapshot escalation policy**:
   - When a gap persists past the reorder window, the receiver issues `RETRANSMIT_REQ(channel, expected_seq, hole_end)` over TCP (chunked to `max_retransmit_chunk`).
   - If the `RecoveryStore` still holds all requested datagrams in its bounded circular buffer, it returns the exact original `MDF1` datagrams (`RecoveryStatus::kOk`).
   - If any requested sequence was evicted from the ring buffer or exceeds `max_retransmit_range`, the server responds with `RecoveryStatus::kUnavailable`. The receiver then escalates to `SYNCING_SNAPSHOT` and requests `SNAPSHOT_REQ(channel)`.
5. **Snapshot-plus-incremental invariant (`as_of_seq = N`)**:
   - Every replay checkpoint is captured atomically after applying packet `N` to the publisher's authoritative book.
   - `RecoveryStore` guarantees `auto_checkpoint_before_eviction`: a new checkpoint is captured automatically before the previous checkpoint's `as_of_seq` can fall behind the tail of the retransmit ring buffer.
   - When the receiver loads a snapshot at `as_of_seq = N`, it replaces its channel book, sets `expected_seq = N + 1`, **discards every buffered incremental with `seq <= N`** (`stale_incrementals_discarded`), replays contiguous buffered packets starting at `N + 1`, and closes any remaining hole between `N + 1` and the earliest staged live packet via TCP retransmit.

## 3. Deterministic State Verification

`ChannelMarketState::state_hash()` computes a canonical 64-bit FNV-1a digest over:
- `channel_id` and `last_applied_seq`
- All non-empty instruments in sorted `instrument_id` order
- Per-instrument trade statistics (`last_trade_price_ticks`, `last_trade_qty`, `trade_count`, `total_volume`)
- All active orders sorted by `order_id` (independent of `std::unordered_map` bucket order)
- Aggregated L2 bid and ask price levels

All integration tests, property tests, the live demo, and every benchmark row verify both full structural equality (`operator==`) and `state_hash()` equality against the publisher's authoritative book.

## 4. Platform Portability & Kernel Tuning Notes

| Feature | Linux | macOS (Darwin) |
|---|---|---|
| IPv4 UDP Multicast (`IP_ADD_MEMBERSHIP`, `IP_MULTICAST_IF`, `IP_MULTICAST_LOOP`) | Supported | Supported |
| Multiple subscribers on same host (`SO_REUSEADDR` + `SO_REUSEPORT`) | Supported | Supported |
| Socket buffer sizing (`SO_RCVBUF` / `SO_SNDBUF`) | Supported (kernel doubles value up to `rmem_max`) | Supported (clamped by `kern.ipc.maxsockbuf`) |
| `recvmmsg()` batch receive | Compiled & active (`#ifdef __linux__`) | Portable `recvfrom()` + `MSG_DONTWAIT` drain fallback |
| `SO_BUSY_POLL` | Supported when kernel permits | Gracefully reports unsupported |
| Thread CPU affinity (`pthread_setaffinity_np`) | Supported | Gracefully reports unsupported |
| Kernel UDP drop counter | `/proc/net/snmp` `Udp: RcvbufErrors` | Reported as `NA` (no fabricated numbers) |

## 5. Explicit Assumptions & Limitations

- **Localhost lab vs. colocation**: Benchmarks run on loopback IPv4 multicast (`127.0.0.1` / `239.255.x.x`) measure OS socket stack, framing, sequencing, and TCP recovery overhead on one host; they are **not** physical NIC / switch / exchange-colocation latencies.
- **No kernel bypass**: DPDK / AF_XDP / Solarflare ef_vi are intentionally out of scope for the core portable POSIX implementation.
- **Custom educational protocol**: `MDF1`/`MDR1` models real exchange multicast+TCP recovery patterns (MoldUDP64 / CME MDP 3.0 concepts) without claiming wire compatibility with any proprietary venue specification.
- **Single-process publisher + recovery store**: In production venues, the snapshot/retransmit feed is typically a separate process fed from a replicated journal; here `RecoveryStore` is thread-safe and decoupled behind `TcpRecoveryServer` so subscribers exercise real TCP framing and network I/O.
