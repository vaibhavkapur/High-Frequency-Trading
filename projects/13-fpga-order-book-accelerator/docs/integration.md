# Integration Guide and Dependencies

## 1. Repository Context and Neighboring Projects

This project lives in `Trading-High-Frequency/projects/13-fpga-order-book-accelerator/` alongside sibling projects under `projects/`:

- `projects/12-kernel-bypass-networking-lab/`
- `projects/14-numa-aware-trading-engine/`
- `projects/15-precision-time-synchronization-platform/`
- `projects/16-order-book-compression-engine/`
- `projects/17-market-microstructure-analytics-engine/`

To keep each project independently buildable with zero cross-project coupling, this accelerator exposes **standalone, zero-external-dependency adapter interfaces** so sibling projects can connect without modifying RTL or core domain logic:

| Integration Boundary | Contract / Interface | Intended Future Consumer / Producer |
|---|---|---|
| Binary Ingress Stream | 20-byte packed `EventPacket` (`fpga_ob::encode_event` / `decode_event`, `order_book_pkg::event_pkt_t`) | `kernel-bypass-networking-lab` (UDP/itch-like decoded feed frames), `order-book-compression-engine` (replayed delta stream) |
| BBO Egress Stream | `BboOutputPacket` (`include/fpga_ob/contracts.hpp`, `order_book_pkg::bbo_pkt_t`) | `numa-aware-trading-engine` (strategy BBO consumer), `market-microstructure-analytics-engine` (spread/imbalance/crossed-book analytics) |
| Timestamp Field | `timestamp_ns` (`uint32_t`, synthetic ingress nanoseconds) | `precision-time-synchronization-platform` (PTP-disciplined ingress timestamps) |
| Typed Domain Library | `libfpga_ob_core` (`include/fpga_ob/rtl_model.hpp`, `reference_book.hpp`) | Standalone C++ harnesses, Verilator co-simulation, or Python/FFI bindings |

## 2. Boundary Separation

- **Hot-path domain & RTL (`rtl/`, `include/fpga_ob/rtl_model.hpp`, `src/rtl_model.cpp`):** Pure deterministic state transitions with zero filesystem, console, or network I/O.
- **I/O & Serialization Adapters (`include/fpga_ob/contracts.hpp`, `src/contracts.cpp`, `src/cli_main.cpp`):** Binary wire packing/unpacking, JSON fixture parsing, structured JSON report output, and replay CLI.
- **Verification & Benchmark Harnesses (`tests/`, `benchmarks/`, `sim/`, `scripts/`):** Independent golden reference models (C++ and Python), differential harnesses, and benchmark drivers.

## 3. Dependencies and Licenses

All source code and fixtures in this project are **100% original** with no private, employer, SAAT, or real market/customer data.

| Dependency | Scope | Required / Optional | License |
|---|---|---|---|
| C++20 Standard Library (`clang++` / `g++`) | Core library, tests, benchmarks, CLI | Required | System toolchain (Apache-2.0 w/ LLVM exception or GPL w/ runtime exception) |
| CMake (`>= 3.20`) | C++ build and CTest orchestration | Required | BSD-3-Clause |
| Python (`>= 3.9`, stdlib only) | Independent reference verifier & validation runner | Required for scripts | PSF-2.0 |
| Verilator (`>= 5.0`) | SystemVerilog RTL C++ compilation & simulation | Optional (EDA) | LGPL-3.0 / Artistic-2.0 |
| cocotb (`>= 1.8.1`, `pytest`) | Python RTL coroutine testbench | Optional (EDA) | BSD-3-Clause |
| Yosys (`>= 0.36`) | Open-source RTL synthesis resource estimation | Optional (EDA) | ISC |

## 4. Compatibility Notes

- Tested locally on **macOS 26.5.1 (Apple M5, arm64)** with **Apple clang 21.0.0**, **CMake 4.5.1**, and **Python 3.9.6**.
- SystemVerilog RTL targets **IEEE 1800-2017** synthesizable subset (no vendor-proprietary IP macros, no SystemVerilog DPI dependencies inside `rtl/`).
- No production exchange gateways, live capital connectors, or credentials are included or enabled.
