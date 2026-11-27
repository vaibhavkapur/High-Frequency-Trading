# Integration & Dependency Boundaries

## 1. Library Boundary Architecture

The project is packaged as two clean CMake static libraries so downstream consumers can link only what they need:

- **`obce_core`** (`include/obce/types.hpp`, `crc32.hpp`, `varint.hpp`, `reference_codec.hpp`, `delta_codec.hpp`, `zstd_adapter.hpp`, `fixture_generator.hpp`):
  - Pure C++20 in-memory codecs and domain validation.
  - Zero file I/O, zero network/socket calls, zero third-party runtime dependencies required.
  - Suitable for embedding directly in low-latency market data recorders, replay engines, or backtesters.
- **`obce_io`** (`include/obce/io_adapter.hpp`):
  - File persistence (`read_binary_file`, `write_binary_file`), JSONL fixture parsing/formatting, and JSON diagnostic counter formatting.
  - Depends on `obce_core`; kept strictly outside the hot path.

## 2. Sibling Projects in `Trading-High-Frequency`

The following neighboring projects exist in this workspace (verified paths under `projects/`) and represent natural integration points via thin, independent adapters (no source code copied between projects):

| Neighboring Project | Verified Path | Integration Pattern |
|---|---|---|
| **Kernel-Bypass Networking Lab** | `projects/12-kernel-bypass-networking-lab/` | Frame `obce::DeltaEncoder` output into UDP/ring-buffer packets for multicast market-data transport experiments; handle packet loss via `GapPolicy::ResyncAtCheckpoint`. |
| **FPGA Order Book Accelerator** | `projects/13-fpga-order-book-accelerator/` | Feed decoded `obce::MarketEvent` streams into the FPGA reference model or testbench via an adapter mapping `price_ticks` / `quantity` / `EventType` into `fpga_ob` order commands. |
| **NUMA-Aware Trading Engine** | `projects/14-numa-aware-trading-engine/` | Use `obce_core` on dedicated I/O/logging cores to compress order-book snapshots off the matching/strategy hot thread. |
| **Precision Time Synchronization Platform** | `projects/15-precision-time-synchronization-platform/` | Supply synchronized nanosecond timestamps (`timestamp_ns`) for `MarketEvent` records before delta encoding. |
| **Market Microstructure Analytics Engine** | `projects/17-market-microstructure-analytics-engine/` | Replay compressed `.obce` L2/L3 archives using `DeltaDecoder::decode_seq_range` to feed order-book imbalance, spread, and trade-flow analytics without storing uncompressed tick logs. |

> No external private employer, proprietary venue feed, or SAAT code is referenced or imported anywhere in this project.

## 3. Dependencies & Licenses

| Dependency | Required? | License | Purpose / Status in Current Environment |
|---|---|---|---|
| **C++20 Standard Library** | Required | Toolchain standard | Core data structures, `<chrono>`, `<vector>`, `<optional>` |
| **CMake >= 3.20** | Required (build) | BSD-3-Clause | Cross-platform build & CTest configuration |
| **libzstd** | Optional (`OBCE_ENABLE_ZSTD`) | BSD-3-Clause / GPLv2 dual | Optional secondary block compression via `ZstdAdapter`. **Not installed in this environment** — CMake auto-detects absence and sets `OBCE_HAVE_ZSTD=0`. |
| **Google Benchmark** | Optional (not required) | Apache-2.0 | Replaced by built-in zero-dependency `obce_bench` (`benchmarks/bench_main.cpp`) for reproducible percentile (`p50`/`p95`/`p99`/`p99.9`) latency & random-access reporting without network downloads. |

## 4. Compatibility

- **Endianness:** Wire format is explicitly little-endian via shift-based `read_*_le` / `write_*_le` helpers (no unaligned pointer casts, portable across x86_64 and ARM64).
- **Determinism:** Synthetic fixtures use a self-contained SplitMix64 + Xoshiro256** PRNG rather than platform-dependent `std::mt19937_64` / `std::uniform_int_distribution`, ensuring byte-identical outputs across macOS, Linux, and Windows.
