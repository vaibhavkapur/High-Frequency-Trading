# Integration Boundaries, Compatibility & Provenance

## 1. Repository Context (`Trading-High-Frequency`)

`projects/17-market-microstructure-analytics-engine/` is an independent, self-contained educational/research project within `Trading-High-Frequency`.

Sibling projects in `projects/`:
- `projects/12-kernel-bypass-networking-lab/`
- `projects/13-fpga-order-book-accelerator/`
- `projects/14-numa-aware-trading-engine/`
- `projects/15-precision-time-synchronization-platform/`
- `projects/16-order-book-compression-engine/`

To remain independently buildable with zero cross-project coupling, MMAE defines clean, standalone adapter boundaries and does **not** copy code from sibling project directories.

---

## 2. Reusable Adapter Boundaries

| Boundary | Interface | Intended Future Integration |
|---|---|---|
| **Event Ingress** | `mmae::Event` (`include/mmae/types.hpp`) + `FeatureEngine::process_event` | Can accept decoded events from `order-book-compression-engine` or pcap/FIX/ITCH decoders by translating into `mmae::Event` structs without altering `FeatureEngine`. |
| **Timestamp Provenance** | `event_ts_ns` (exchange time) + `recv_ts_ns` (capture time) | Compatible with PTP-synchronized hardware capture timestamps from `precision-time-synchronization-platform`. |
| **Order Book Core** | `mmae::IOrderBook` (`include/mmae/order_book.hpp`) | Pure C++20 static library (`libmmae_core.a`) with zero I/O or networking dependencies; embeddable in low-latency replay or NUMA-pinned workers (`numa-aware-trading-engine`). |
| **Offline Analytics** | `mmae.v1` CSV/JSON contracts (`docs/contracts.md`) | Decoupled schema allows downstream Python/Polars/Arrow research pipelines to consume `features.csv` and `diagnostics.json` independently of C++ compilation. |

---

## 3. Dependency & License Inventory

All source code in this project is original. No proprietary employer, SAAT, customer, or live trading credentials/data are present.

| Component | Dependency | Version / Pin | License | Required or Optional |
|---|---|---|---|---|
| C++20 Core, CLI, Tests, Benchmark | C++20 Standard Library (`libc++` / `libstdc++`) | ISO C++20 | Apache-2.0 with LLVM Exception / GPLv3 with GCC Runtime Exception | Required |
| Build System | CMake | `>= 3.20` (tested on `4.5.1`) | BSD-3-Clause | Required |
| Offline Evaluation & Fixtures | Python Standard Library | `>= 3.9` (tested on `3.9.6`) | PSF-2.0 | Required |
| Columnar CSV Acceleration | `polars` (`requirements.txt`) | `1.17.1` | MIT | Optional (clean stdlib fallback when absent) |
| Test Runner Alternative | `pytest` (`requirements.txt`) | `8.3.4` | MIT | Optional (`python3 -m unittest` supported natively) |

---

## 4. Fixture Provenance

All fixtures in `fixtures/` are 100% synthetic and deterministically generated/authored for educational and verification purposes:
- `fixtures/hand_worked_l3.csv` & `fixtures/hand_worked_l3_expected.json`: Hand-authored 12-event L3 + TRADE reference sequence with exact analytical derivations.
- `fixtures/edge_cases_crossed_stale_gaps.csv`: Hand-authored boundary sequence exercising locked/crossed books, heartbeat staleness, and resolved/unresolved sequence gaps.
- `fixtures/invalid_events.csv`: Hand-authored malformed and constraint-violating event rows.
- `fixtures/synthetic_signed_flow_config.json`: Fixed-seed (`seed=42`) configuration for the synthetic signed-flow generator (`python/mmae_analytics/fixtures.py`).

No exchange-proprietary market data, real order flow, or customer records are included.
