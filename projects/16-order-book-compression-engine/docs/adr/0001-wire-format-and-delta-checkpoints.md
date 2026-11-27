# ADR-0001: Fixed-Point Reference Encoding, Bitmask Delta/Varint Framing, and Periodic Checkpoints

- **Status:** Accepted
- **Date:** 2026-11-27
- **Project:** Order Book Compression Engine (`projects/16-order-book-compression-engine/`)

## Context

High-frequency L2/L3 market data feeds generate millions of order book events per second. Storing or replaying uncompressed structs wastes memory bandwidth and disk I/O, while generic stream compressors (e.g., gzip, zstd over large blocks) hurt random-access seek latency and fail to exploit domain-specific structure (unit sequence increments, small timestamp/price/quantity deltas, repeated symbol/venue IDs).

Additionally, financial prices must never suffer binary floating-point rounding drift, corrupted frames must be detected before corrupting downstream book state, and streams must remain recoverable when an intermediate delta frame is lost or damaged.

## Decision

1. **Fixed-point signed 64-bit prices:** Represent `price_ticks` as `int64_t` scaled by `10^(-price_decimals)` (default `price_decimals = 8`, explicit in the 24-byte `StreamHeader`). Signed `int64_t` supports negative spreads/yields without special-case encodings.
2. **Canonical little-endian reference binary format:** Define a fixed-width reference format (`47` bytes for schema v1, `59` bytes for schema v2) with explicit little-endian byte serialization, framed with a 20-byte header and a 4-byte Castagnoli CRC32C checksum. This serves both as the baseline benchmark reference and as the anchor format for checkpoint frames.
3. **Bitmask + ZigZag + LEB128 varint delta encoding:** Encode each delta event with a 1-byte core presence bitmask (`seq_id != prev + 1`, timestamp, symbol, order_id, price, quantity, metadata/level, v2 extensions) followed only by changed fields as ZigZag + LEB128 varints. Compute deltas with modular 64-bit unsigned subtraction (`signed_delta_wrap`) so extreme swings (`INT64_MIN` to `INT64_MAX`) roundtrip without C++ signed overflow undefined behavior.
4. **Periodic checkpoints + seekable IndexFooter:** Emit a self-contained `Checkpoint` frame every `checkpoint_interval` events (default `256`), and optionally append an `IndexFooter` frame plus 12-byte EOF trailer (`OIDX`) containing checkpoint offsets and sequence ranges. Random access performs binary search on the checkpoint index (`O(log C)`) and decodes at most `checkpoint_interval` events forward.
5. **Explicit gap & recovery policies:** Every `Delta` frame header carries its authoritative `first_seq_id`. If a preceding delta frame was dropped in transit, `prev.seq_id + delta != frame.first_seq_id` triggers immediate detection (`SequenceGap`) instead of silently applying deltas onto stale state. Under `GapPolicy::ResyncAtCheckpoint`, the decoder skips corrupted/orphaned delta chains and scans forward to the next CRC-verified `Checkpoint` frame.
6. **Optional secondary zstd adapter:** Keep `libzstd` strictly optional outside the hot-path core (`ZstdAdapter`). When not installed, the library builds cleanly with zero external dependencies and returns `ErrorCode::UnsupportedFeature` explicitly if invoked.

## Consequences

- **Positive:** 4.4x–5.0x lossless compression on realistic L2/L3 order flow, sub-60 ns/event p99 encode/decode latency on Apple Silicon, bounded `O(checkpoint_interval)` random access, and deterministic zero-dependency reproducibility.
- **Trade-offs:** Smaller `checkpoint_interval` lowers random-access cost and blast radius on frame loss at the expense of slightly higher bytes/event; larger `checkpoint_interval` improves compression ratio slightly while linearly increasing worst-case random-access scan cost.
- **Limitations:** Does not apply entropy coding (Huffman/ANS) to delta bitmasks or varint payloads unless paired with the optional `ZstdAdapter` when `libzstd` is installed.
