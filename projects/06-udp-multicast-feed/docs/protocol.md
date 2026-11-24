# Wire Protocol Specification (`MDF1` & `MDR1`)

All multi-byte integer fields on the wire are **big-endian (network byte order)**. All decoding is alignment-free and strictly bounds-checked.

---

## 1. UDP Multicast Market Data Packet (`MDF1`)

Packet framing is separated from individual market events so one UDP datagram can carry `0..N` market events up to the IPv4 MTU payload budget (`1,472` bytes).

### 1.1 Packet Header (36 bytes fixed)

| Offset | Size (B) | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | `uint32` | Constant `0x4D444631` (`"MDF1"`) |
| 4 | 2 | `version` | `uint16` | Constant `1` |
| 6 | 2 | `flags` | `uint16` | Bitmask: `0x0001` = Heartbeat, `0x0002` = Retransmit, `0x0004` = EndOfBatch |
| 8 | 2 | `channel_id` | `uint16` | Logical feed channel identifier |
| 10 | 2 | `msg_count` | `uint16` | Number of framed market events in the payload |
| 12 | 4 | `payload_len` | `uint32` | Byte length of the serialized event payload (`<= 1436`) |
| 16 | 8 | `seq_num` | `uint64` | Strictly monotonic 1-based per-channel packet sequence number |
| 24 | 8 | `send_ts_ns` | `uint64` | Publisher `CLOCK_MONOTONIC` timestamp in nanoseconds |
| 32 | 4 | `checksum` | `uint32` | IEEE 802.3 CRC32 over `[0..31] + 0x00000000 + payload` |

### 1.2 Heartbeat Semantics

When `(flags & 0x0001) != 0`:
- `msg_count` MUST be `0` and `payload_len` MUST be `0`.
- `seq_num` carries the publisher's `last_published_seq` on `channel_id` and does **not** consume a sequence number.
- If a receiver sees a heartbeat with `seq_num >= expected_seq`, it detects silent tail-packet loss and immediately triggers recovery for `[expected_seq .. seq_num]`.

### 1.3 Event Framing Inside Payload

Each of the `msg_count` events is prefixed with a 2-byte record length:

```text
+--------------------+------------------+---------------------------+
| record_len (u16)   | event_type (u8)  | event body (record_len-1) |
+--------------------+------------------+---------------------------+
```

| `event_type` | Name | `record_len` | Body Layout |
|---|---|---:|---|
| `'A'` (`0x41`) | `AddOrder` | 26 | `instrument_id(u32)`, `order_id(u64)`, `side(u8: 0=Bid, 1=Ask)`, `price_ticks(i64)`, `qty(u32)` |
| `'M'` (`0x4D`) | `ModifyOrder` | 25 | `instrument_id(u32)`, `order_id(u64)`, `new_price_ticks(i64)`, `new_qty(u32)` |
| `'C'` (`0x43`) | `CancelOrder` | 17 | `instrument_id(u32)`, `order_id(u64)`, `cancel_qty(u32)` (`0` or `>= qty` deletes) |
| `'T'` (`0x54`) | `Trade` | 25 | `instrument_id(u32)`, `resting_order_id(u64)`, `exec_price_ticks(i64)`, `exec_qty(u32)` |
| `'X'` (`0x58`) | `ClearBook` | 5 | `instrument_id(u32)` (`0` clears all instruments on channel) |

All events in packet `seq_num = S` are applied atomically as transaction `S`. Any trailing bytes after decoding `msg_count` events cause `DecodeStatus::kBadEventCount`.

---

## 2. TCP Snapshot & Retransmission Protocol (`MDR1`)

Every message sent over TCP uses a 12-byte frame header followed by `payload_len` bytes (`<= 32 MiB`):

| Offset | Size (B) | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | `uint32` | Constant `0x4D445231` (`"MDR1"`) |
| 4 | 2 | `version` | `uint16` | Constant `1` |
| 6 | 2 | `msg_type` | `uint16` | `1`=RetransmitReq, `2`=RetransmitResp, `3`=SnapshotReq, `4`=SnapshotResp |
| 8 | 4 | `payload_len` | `uint32` | Byte length of frame payload |

### 2.1 `RetransmitRequest` (`msg_type = 1`, 18 bytes)

- `channel_id` (`u16`), `from_seq` (`u64`), `to_seq` (`u64`, inclusive).

### 2.2 `RetransmitResponse` (`msg_type = 2`)

- `channel_id` (`u16`), `status` (`u16`: `0=OK`, `1=UNAVAILABLE`, `2=UNKNOWN_CHANNEL`, `3=INVALID_REQUEST`)
- `oldest_available_seq` (`u64`), `latest_seq` (`u64`), `latest_snapshot_seq` (`u64`)
- `packet_count` (`u32`)
- Repeated `packet_count` times: `pkt_len` (`u32`) + raw `MDF1` datagram bytes.

### 2.3 `SnapshotRequest` (`msg_type = 3`, 2 bytes)

- `channel_id` (`u16`).

### 2.4 `SnapshotResponse` (`msg_type = 4`)

- `status` (`u16`), `channel_id` (`u16`), `as_of_seq` (`u64`), `checkpoint_id` (`u64`)
- `order_count` (`u32`), `instrument_stat_count` (`u32`)
- Repeated `order_count` times (25 bytes each): `instrument_id(u32)`, `order_id(u64)`, `side(u8)`, `price_ticks(i64)`, `qty(u32)`
- Repeated `instrument_stat_count` times (32 bytes each): `instrument_id(u32)`, `last_trade_price_ticks(i64)`, `last_trade_qty(u32)`, `trade_count(u64)`, `total_volume(u64)`
- Trailing `crc32` (`u32`) over all preceding bytes of the `SnapshotResponse` payload.
