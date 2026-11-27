# Data Contracts & Wire Specification

## 1. Canonical Market Event Schema (`MarketEvent`)

All events represent L2 (price-level aggregated) or L3 (order-by-order) order book updates.

| Field | C++ Type | Units / Semantics | Schema Version | Validation Rules |
|---|---|---|---|---|
| `seq_id` | `uint64_t` | Monotonic per-stream sequence number | v1, v2 | Must be `>= 1`; strictly increasing within a stream |
| `timestamp_ns` | `uint64_t` | Exchange timestamp in nanoseconds since Unix epoch | v1, v2 | Any `uint64_t` |
| `symbol_id` | `uint32_t` | Compact integer instrument identifier | v1, v2 | Must be `>= 1` |
| `order_id` | `uint64_t` | Venue order identifier | v1, v2 | Required `> 0` for L3 order events (`Add`/`Modify`/`Cancel`/`Execute`); `0` allowed for `L2` and `BookClear` |
| `price_ticks` | `int64_t` | Signed fixed-point price in `10^(-price_decimals)` units (default `10^-8`) | v1, v2 | Full `int64_t` range (supports negative spreads/yields) |
| `quantity` | `int64_t` | Lot / share / contract quantity | v1, v2 | Must be `>= 0` |
| `type` | `EventType` (`uint8_t`) | `1=AddOrder`, `2=ModifyOrder`, `3=CancelOrder`, `4=ExecuteOrder`, `5=L2Update`, `6=L2Snapshot`, `7=BookClear` | v1, v2 | Must be in `[1, 7]` and compatible with `scope` |
| `side` | `Side` (`uint8_t`) | `0=Unknown/NONE`, `1=Bid`, `2=Ask` | v1, v2 | Must be `Bid` or `Ask` for L3 order events |
| `scope` | `BookScope` (`uint8_t`) | `2=L2`, `3=L3` | v1, v2 | L3 events (`1..4`) require `L3`; L2 events (`5..6`) require `L2`; `BookClear` allows either |
| `level_index` | `uint16_t` | 0-based L2 depth level (`0` = top of book) | v1, v2 | `0..65535` |
| `venue_id` | `uint16_t` | Venue / matching engine ID | v2 | Zeroed on v1 streams |
| `flags` | `uint16_t` | Event bit flags (e.g., aggressive/passive, snapshot end) | v2 | Zeroed on v1 streams |
| `match_id` | `uint64_t` | Trade execution match identifier | v2 | Zeroed on v1 streams |

## 2. Schema Evolution Rules

- **Backward compatibility (v1 -> v2 reader):** A stream with `StreamHeader.schema_version == 1` is decoded seamlessly into `MarketEvent` by the v2 decoder; `venue_id`, `flags`, and `match_id` default deterministically to `0`, and `DiagnosticCounters.schema_v1_upgrades` is incremented per upgraded event.
- **Strict v1 enforcement:** A stream declaring `schema_version == 1` must not set the v2 extension bit (`0x80`) in delta masks; violating frames fail with `ErrorCode::CorruptFrame`.
- **Forward rejection:** Any unknown schema version (e.g., `0`, `3`, `99`) fails immediately with `ErrorCode::UnsupportedSchemaVersion` — never silently misinterpreting unknown wire fields.

## 3. Wire Format Contracts

### Stream Header (24 bytes, little-endian)

| Offset | Size | Field | Value / Constraint |
|---|---|---|---|
| `0` | 4 | `magic` | `0x4543424F` (`"OBCE"`) |
| `4` | 2 | `schema_version` | `1` or `2` |
| `6` | 1 | `codec` | `1=ReferenceBinary`, `2=DeltaVarint`, `3=DeltaVarintZstd` |
| `7` | 1 | `price_decimals` | `0..18` (default `8`) |
| `8` | 4 | `checkpoint_interval` | `1..65536` |
| `12` | 4 | `flags` | Bit 0: `1` if `IndexFooter` appended |
| `16` | 8 | `stream_id` | Arbitrary caller stream identifier |

### Frame Header (20 bytes) + Payload + CRC32C (4 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0` | 2 | `frame_magic` | `0x4F42` |
| `2` | 1 | `frame_type` | `1=ReferenceBatch`, `2=Checkpoint`, `3=Delta`, `4=IndexFooter` |
| `3` | 1 | `frame_flags` | Reserved (`0`) |
| `4` | 4 | `payload_length` | Bytes of payload (`<= 16 MiB`) |
| `8` | 4 | `event_count` | Events in frame (`<= 65536`; checkpoint entries for `IndexFooter`) |
| `12` | 8 | `first_seq_id` | Authoritative `seq_id` of first event (or byte offset of `IndexFooter`) |
| `20` | `payload_length` | `payload` | Fixed-width records (`47 B` v1 / `59 B` v2), delta records, or 32-byte index entries |
| `20+L` | 4 | `crc32c` | Castagnoli CRC32C over bytes `[0 .. 20+L)` |

## 4. Error & Validation Contract

All fallible APIs return `Status` or `Result<T>` with explicit `ErrorCode` values:

| ErrorCode | Trigger Condition |
|---|---|
| `INVALID_ARGUMENT` | Invalid `CodecConfig` (`checkpoint_interval == 0`, `price_decimals > 18`, `delta_frame_events == 0`), invalid seek range, encoder pushed after `finish()` |
| `INVALID_EVENT` | `seq_id == 0`, `symbol_id == 0`, `quantity < 0`, invalid enum, L2/L3 scope mismatch, or L3 order event with `order_id == 0` / `side == Unknown` |
| `TRUNCATED_INPUT` | Stream header, frame header, payload, varint, or CRC truncated before expected byte count |
| `INVALID_MAGIC` | Stream magic `!= 0x4543424F` or frame magic `!= 0x4F42` |
| `UNSUPPORTED_SCHEMA_VERSION` | Schema version other than `1` or `2` |
| `UNSUPPORTED_CODEC` | Unknown codec ID or mismatched decoder invoked |
| `UNSUPPORTED_FEATURE` | `ZstdAdapter` called when built without `libzstd` |
| `CRC_MISMATCH` | Computed CRC32C differs from frame trailer CRC32C |
| `SEQUENCE_GAP` | `seq_id > prev.seq_id + 1` under `StrictReject`, or dropped intermediate delta frame (`decoded_first_seq != frame.first_seq_id`) |
| `SEQUENCE_REGRESSION` | `seq_id <= prev.seq_id` (always rejected under all policies) |
| `VARINT_OVERFLOW` | LEB128 varint exceeds 64 bits (10th byte `> 0x01`) |
| `FRAME_LIMIT_EXCEEDED` | Frame exceeds `16 MiB` payload or `65,536` events |
| `CORRUPT_FRAME` | Structural inconsistency (e.g., trailing unconsumed bytes in delta frame, invalid packed metadata, delta before checkpoint) |
| `CHECKPOINT_NOT_FOUND` | Requested `seq_id` not found in indexed stream |
| `IO_ERROR` | Missing or unreadable/unwritable file in `obce_io` |

## 5. Canonical Fixtures

- [`fixtures/valid_l2_l3_v1.jsonl`](../fixtures/valid_l2_l3_v1.jsonl) — 8 deterministic L2/L3 schema v1 events.
- [`fixtures/valid_l2_l3_v2.jsonl`](../fixtures/valid_l2_l3_v2.jsonl) — 5 deterministic L2/L3 schema v2 events (including negative `price_ticks`).
- [`fixtures/invalid_events.jsonl`](../fixtures/invalid_events.jsonl) — 7 contract-violating events verifying rejection with `INVALID_EVENT`.
- [`fixtures/expected_reference_summary.json`](../fixtures/expected_reference_summary.json) — Independent expected sizes and frame counts.
