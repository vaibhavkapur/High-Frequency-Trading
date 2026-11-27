# Architecture — Order Book Compression Engine

## 1. Component Overview

The engine strictly separates hot-path domain codecs (`obce_core`), optional external compression adapters (`ZstdAdapter`), I/O & JSONL serialization (`obce_io`), CLI tooling (`obce_cli`), and benchmark/test harnesses.

```mermaid
flowchart LR
    subgraph Inputs["Inputs & Fixtures"]
        GEN["Deterministic Synthetic Generator<br/>(SplitMix64 / Xoshiro256**)"]
        JSONL["Canonical JSONL Fixtures<br/>(v1 / v2 / invalid)"]
    end

    subgraph IO["I/O Adapter Layer (obce_io)"]
        LOAD["JSONL & Binary File I/O<br/>(io_adapter.cpp)"]
    end

    subgraph Core["Hot-Path Domain Core (obce_core — Zero External Deps)"]
        VAL["Schema & Invariant Validator<br/>(v1 / v2 contracts)"]
        REF["Reference Binary Codec<br/>(Fixed-width LE + CRC32C)"]
        DENC["DeltaEncoder<br/>(Bitmask + ZigZag + LEB128 + Checkpoints)"]
        DDEC["DeltaDecoder<br/>(Full Decode, Resync, Seek/Range)"]
        CRC["CRC32C Engine & Varint Codec<br/>(Wrap-safe modular deltas)"]
    end

    subgraph Opt["Optional Adapter"]
        ZSTD["ZstdAdapter<br/>(Compile-time optional libzstd)"]
    end

    subgraph Consumers["CLI, Tests & Benchmarks"]
        CLI["obce_cli<br/>(demo / encode / decode / inspect / seek)"]
        TESTS["CTest Suites<br/>(5 suites, 18 test cases)"]
        BENCH["obce_bench<br/>(Compression, p99 Latency, Seek Cost)"]
    end

    GEN --> VAL
    JSONL --> LOAD --> VAL
    VAL --> REF
    VAL --> DENC
    REF --> CRC
    DENC --> CRC
    CRC --> DDEC
    DENC -. optional .-> ZSTD
    DDEC --> CLI
    DDEC --> TESTS
    DDEC --> BENCH
```

## 2. Wire Format Layout

```mermaid
flowchart LR
    SH["StreamHeader<br/>24 B (magic 'OBCE', schema, codec, decimals, cp_interval)"]
    CP0["Checkpoint #0<br/>20 B hdr + Full Ref Record + 4 B CRC32C"]
    D0["Delta Frame(s)<br/>20 B hdr + Packed Delta Events + 4 B CRC32C"]
    CP1["Checkpoint #1<br/>20 B hdr + Full Ref Record + 4 B CRC32C"]
    D1["Delta Frame(s)<br/>20 B hdr + Packed Delta Events + 4 B CRC32C"]
    IDX["IndexFooter Frame<br/>20 B hdr + N*32 B Entries + 4 B CRC32C"]
    TRL["EOF Trailer<br/>12 B (footer_offset + 'OIDX')"]

    SH --> CP0 --> D0 --> CP1 --> D1 --> IDX --> TRL
```

### Frame Integrity & Bounds
- Every frame starts with a 20-byte header (`frame_magic=0x4F42`, `frame_type`, `flags`, `payload_length`, `event_count`, `first_seq_id`) and ends with a 4-byte Castagnoli CRC32C computed over `[header || payload]`.
- Bounded capacity guards reject any frame claiming `payload_length > 16 MiB` or `event_count > 65,536` before allocating or reading payload memory.

### Delta Encoding Mechanics
Each delta event compares against the immediately preceding event (`prev`):
- **1-byte `core_mask`**:
  - Bit 0: `seq_id != prev.seq_id + 1` (0 bytes on wire for contiguous sequence IDs)
  - Bit 1: `timestamp_ns != prev.timestamp_ns` (ZigZag LEB128)
  - Bit 2: `symbol_id != prev.symbol_id` (ZigZag LEB128)
  - Bit 3: `order_id != prev.order_id` (ZigZag LEB128)
  - Bit 4: `price_ticks != prev.price_ticks` (wrap-safe ZigZag LEB128)
  - Bit 5: `quantity != prev.quantity` (wrap-safe ZigZag LEB128)
  - Bit 6: metadata/level changed (1 packed byte + optional LEB128 `level_index`)
  - Bit 7: schema v2 extension changed (1-byte `ext_mask` + changed `venue_id`, `flags`, `match_id`)

## 3. Core Invariants

1. **Lossless roundtrip:** For every valid event stream, `decode(encode(events)) == events` field-for-field and yields identical canonical CRC32C digests.
2. **No silent corruption:** Every CRC mismatch, truncation, varint overflow (>64 bits), invalid enum value, or sequence regression fails explicitly with a typed `ErrorCode`.
3. **No silent delta drift on frame loss:** Each `Delta` frame header embeds `first_seq_id`. If an earlier delta frame was dropped, the decoded first event's `seq_id` mismatches `frame.first_seq_id`, halting delta application immediately rather than applying deltas to stale price/order state.
4. **Bounded random-access scan:** With `IndexFooter` enabled, `seek_by_seq_id` performs `O(log C)` binary search over checkpoints and decodes at most `checkpoint_interval` events.
5. **No signed overflow UB:** Signed 64-bit deltas use modular unsigned arithmetic (`uint64_t(cur) - uint64_t(prev)`), guaranteeing safe transitions across `[INT64_MIN, INT64_MAX]`.

## 4. Engineering Trade-Offs

| Dimension | Choice Made | Alternative Considered | Rationale |
|---|---|---|---|
| **Checkpoint interval** | Configurable (`64..4096`, default `256`) | Fixed interval or single stream-start anchor | Lets operators tune space (~12–13.5 B/ev) vs random-access p99 (2.5–137 µs) and blast radius on frame loss |
| **Delta encoding** | Bitmask + ZigZag LEB128 varint | Columnar SIMD bit-packing / ANS | Low per-event latency (<60 ns p99), streaming-friendly incremental push API, simple auditability |
| **Checksum** | CRC32C (Castagnoli) per frame | Stream-wide SHA-256 or no per-frame checksum | Detects corruption at frame granularity enabling checkpoint resync; fast table-driven deterministic C++20 |
| **Optional zstd** | Compile-time isolated `ZstdAdapter` | Hard dependency on `libzstd` | Preserves zero-dependency clean builds while exposing an explicit secondary compression hook when available |
