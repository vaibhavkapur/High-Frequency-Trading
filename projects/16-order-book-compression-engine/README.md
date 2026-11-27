# Order Book Compression Engine

A zero-dependency C++20 lossless compression engine for L2 and L3 order book event streams, combining a fixed-point canonical binary reference format, bitmask + ZigZag/LEB128 delta encoding, periodic checkpoints with an `O(log C)` seekable index, CRC32C frame integrity verification, schema evolution (v1/v2), and automatic recovery from missing or corrupted delta frames.

> **Scope notice:** Independently developed educational and research prototype using synthetic fixtures only. No live trading, real customer records, credentials, or proprietary venue data are included.

---

## Key Capabilities

- **Fixed-point canonical reference binary codec (`ReferenceCodec`):** Little-endian fixed-width serialization (`47 B` schema v1 / `59 B` schema v2) with signed 64-bit fixed-point `price_ticks` (`10^-8` default) and per-frame Castagnoli CRC32C.
- **Delta + ZigZag/LEB128 codec with periodic checkpoints (`DeltaEncoder` / `DeltaDecoder`):** Presence bitmask skips unchanged fields (0 bytes for contiguous `seq_id`), wrap-safe modular 64-bit deltas (no signed overflow UB even on `INT64_MIN <-> INT64_MAX`), periodic self-contained `Checkpoint` frames, and optional `IndexFooter` for bounded random access (`seek_by_seq_id`, `decode_seq_range`).
- **Resilience & schema evolution:** Explicit `GapPolicy` modes (`StrictReject`, `AllowAndFlag`, `ResyncAtCheckpoint`), authoritative per-frame `first_seq_id` checks to catch dropped delta frames before corrupting book state, transparent v1 -> v2 upgrade with zeroed defaults, and strict rejection of unknown schema versions.
- **Clean boundaries:** Hot-path domain core (`obce_core`) isolated from I/O (`obce_io`), optional secondary `libzstd` adapter (`ZstdAdapter`), CLI (`obce_cli`), tests, and benchmarks (`obce_bench`).

---

## Quickstart

### Prerequisites
- C++20 compiler (`clang++` >= 15 or `g++` >= 11)
- `cmake` >= 3.20
- Optional: `libzstd` (auto-detected if installed; omitted cleanly otherwise)

### Build, Test, Demo, and Benchmark (Single Command)
```bash
cd projects/16-order-book-compression-engine
./scripts/run_all.sh
```

### Manual Step-by-Step Commands
```bash
# 1. Configure & build in Release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 2. Run all 5 CTest suites (22 test cases)
ctest --test-dir build --verbose --output-on-failure

# 3. Run reproducible demo
./build/obce_cli demo --events 10000 --seed 20261127 --checkpoint 256 --schema 2

# 4. Run full compression, latency & random-access benchmark
./build/obce_bench --events 50000 --json reports/benchmark_results.json --csv reports/benchmark_summary.csv
```

---

## Example Demo Output

Captured from [`reports/demo_output.txt`](reports/demo_output.txt) on Apple M5 (Release, C++20):

```text
=== Order Book Compression Engine Demo ===
Workload:              l3_order_flow (synthetic, seed=20261127, events=10000)
Schema version:        v2 (price_decimals=8)
Checkpoint interval:   256 (40 checkpoints)
Optional zstd adapter: unavailable (built without libzstd)
------------------------------------------
Reference binary:      593792 bytes (59.38 bytes/event)
Delta+Varint stream:   123314 bytes (12.33 bytes/event)
Compression ratio:     4.82x vs reference binary
Input digest (CRC32C): 0x1756E2FE
Decoded digest:        0x1756E2FE (MATCH=YES)
Random-access seek:    seq_id=5001 found after scanning 138 events (3 frames)
Corrupt-frame strict:  rejected with CRC_MISMATCH
Corrupt-frame resync:  recovered 9745/10000 events (resync_recoveries=1)
```

---

## Measured Benchmark Highlights

Observed on **Apple M5 (arm64, Darwin 25.5.0, AppleClang 21.0.0, Release `-O3`)** using **synthetic deterministic workloads** (50,000 events per workload, `std::chrono::steady_clock`). These are actual measurements on synthetic data, not guarantees for real venue feeds. Full raw results are stored in [`reports/benchmark_results.json`](reports/benchmark_results.json) and [`reports/benchmark_summary.csv`](reports/benchmark_summary.csv).

| Synthetic Workload | Codec | Checkpoint | Bytes/Event | Ratio vs Ref | Encode p99 (ns/ev) | Decode p99 (ns/ev) | Random Seek p99 (µs) |
|---|---|---:|---:|---:|---:|---:|---:|
| `l3_order_flow` | `reference_binary_v2` | — | 59.38 | 1.00x | 137.76 | 143.55 | — |
| `l3_order_flow` | `delta_varint_v2` | 64 | 13.54 | 4.38x | 70.50 | 52.57 | 2.54 |
| `l3_order_flow` | `delta_varint_v2` | 256 | 12.33 | 4.81x | 52.42 | 40.04 | 9.76 |
| `l3_order_flow` | `delta_varint_v2` | 1024 | 12.03 | 4.94x | 51.76 | 39.42 | 34.58 |
| `l3_order_flow` | `delta_varint_v2` | 4096 | 11.95 | 4.97x | 64.40 | 49.91 | 137.58 |
| `l2_top_of_book` | `delta_varint_v2` | 256 | 12.40 | 4.79x | 64.36 | 49.07 | 8.63 |
| `volatile_bursty` | `delta_varint_v2` | 256 | 23.18 | 2.56x | 103.06 | 80.89 | 15.88 |

> Note: Delta+Varint achieves faster per-event encode/decode p99 than the fixed-width reference codec on these workloads primarily because per-frame CRC32C hashes ~2.5x–4.9x fewer bytes.

---

## Project Layout & Documentation

- [`plan.md`](plan.md) — Original implementation plan and milestone exit gates.
- [`docs/architecture.md`](docs/architecture.md) — Component diagram, wire layout, invariants, and trade-offs.
- [`docs/adr/0001-wire-format-and-delta-checkpoints.md`](docs/adr/0001-wire-format-and-delta-checkpoints.md) — Architecture Decision Record for wire format and checkpoints.
- [`docs/contracts.md`](docs/contracts.md) — Canonical schemas, fixed-point units, wire specification, and error codes.
- [`docs/integration.md`](docs/integration.md) — Library boundaries, sibling `Trading-High-Frequency` integration points, and dependency licenses.
- [`docs/runbook.md`](docs/runbook.md) — Operator CLI guide, diagnostic counters, and recovery from missing/corrupt delta frames.
- [`reports/acceptance.md`](reports/acceptance.md) — Full acceptance checklist mapped to measured test and benchmark evidence.
- [`reports/`](reports/) — Machine-readable benchmark JSON/CSV, environment metadata, test output, and demo output.
- [`fixtures/`](fixtures/) — Canonical v1/v2 valid fixtures, invalid contract fixtures, and expected summary.
- [`.github/workflows/ci.yml`](.github/workflows/ci.yml) — CI build, test, demo, and benchmark smoke workflow.

---

## Assumptions & Known Limitations

1. **Workload-dependent compression:** Compression trades CPU for storage/bandwidth; high-entropy or randomized payloads (e.g., `volatile_bursty` at 2.56x vs `l3_order_flow` at 4.81x) compress less than bursty single-symbol order flow.
2. **Optional `libzstd` not run here:** `ZstdAdapter` is compile-time optional and was built with `OBCE_HAVE_ZSTD=0` because `libzstd` is not installed in the validation environment (explicitly returns `UNSUPPORTED_FEATURE`).
3. **Checkpoint recovery blast radius:** In `ResyncAtCheckpoint` mode, losing a delta frame forfeits the remaining delta frames until the next `Checkpoint` frame (at most `checkpoint_interval - 1` events). Tune `checkpoint_interval` based on acceptable recovery window vs byte overhead.
4. **No hardware CRC32C intrinsics:** Uses portable deterministic table-driven Castagnoli CRC32C in pure C++20; adding ARMv8 `__crc32cd` / x86 `_mm_crc32_u64` fast paths would further reduce frame checksum latency.
