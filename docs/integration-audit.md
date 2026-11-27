# Phase 0 — Integration Audit & Baseline Inventory

**Date audited:** 2026-11-27  
**Baseline commit SHA:** `8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89`  
**Repository:** https://github.com/vaibhavkapur/High-Frequency-Trading

This document records directly observed build, test, CLI, and API inventory results for the ten standalone C++20 projects in [vaibhavkapur/High-Frequency-Trading](https://github.com/vaibhavkapur/High-Frequency-Trading/tree/main/projects) (pinned baseline commit `8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89`). No claims from module READMEs are repeated without local execution.

---

## 1. Environment & Toolchain Provenance

| Attribute | Measured Value |
|---|---|
| Operating System | macOS 26.5.1 (Build `25F80`), Darwin Kernel 25.5.0 |
| Architecture | `arm64` (Apple Silicon) |
| CPU | Apple M5 (10 logical cores) |
| RAM | 16 GiB (`17,179,869,184` bytes) |
| C++ Compiler | AppleClang 21.0.0 (`clang-2100.1.1.101`) |
| CMake | 4.5.1 |
| Git | 2.50.1 (Apple Git-155) |
| Python | 3.9.6 |
| SQLite3 | Found (system SDK; enabled in Project 10) |
| Preinstalled GTest / Catch2 / Google Benchmark | Not installed locally via Homebrew (projects 01, 02, 03, 04, 07 fetch via CMake `FetchContent` when tests/gbench are enabled) |
| Build mode | `Release` (`CMAKE_CXX_STANDARD=20`, `CMAKE_CXX_EXTENSIONS=OFF`) |

---

## 2. Standalone Build, Test & Baseline Matrix

All ten projects were configured and compiled out-of-source under `build/baseline/01` .. `build/baseline/10`.

| # | Module | Library Target(s) | Core Lib + CLI Build | Offline `ctest` Result | Notes / External Deps |
|---|---|---|---|---|---|
| 01 | `01-matching-engine` | `me_core` (INTERFACE), `me_support`, `me_alloc_counter` | **PASS** (0 warnings) | **SKIPPED (offline)** | Tests require GTest (`FetchContent`); benches require Google Benchmark. CLI `me_cli` verified against `sample_session.expected.txt` (byte-identical for both `pool` and `map`). |
| 02 | `02-itch-feed-handler` | `itch` (STATIC) | **PASS** (0 warnings) | **SKIPPED (offline)** | Tests require Catch2 v3.7.1 (`FetchContent`). `itch_gen` + `itch_replay` verified cleanly. |
| 03 | `03-ouch-order-gateway` | `ouch_core`, `ouch_sim` (STATIC) | **PASS** (0 warnings) | **SKIPPED (offline)** | Tests require GTest (`FetchContent`). Loopback TCP socket tests require localhost bind permission. |
| 04 | `04-event-driven-backtester` | `hftbt` (STATIC) | **PASS** (0 warnings) | **SKIPPED (offline)** | Tests unconditionally call `FetchContent` for GTest (no `find_package`). `hftbt gen`/`info` verified cleanly. |
| 05 | `05-lock-free-messaging` | `lfm` (`lfm::lfm`, STATIC) | **PASS** (0 warnings) | **PASS (6/6, 10.52s)** | Configured with `-DLFM_BUILD_GBENCH=OFF`. Zero external deps for tests; `market_data_pipeline` accounting verified OK. |
| 06 | `06-udp-multicast-feed` | `mdf_core` (`mdf::core`, STATIC) | **PASS** (0 warnings) | **5/6 PASS in sandbox** | `test_wire`, `test_order_book`, `test_sequencer`, `test_fault_recovery`, `test_fuzz_property` PASS offline. `test_multicast_integration` requires loopback UDP multicast & TCP bind permission (blocked inside network-isolated sandbox). |
| 07 | `07-smart-order-router` | `sor_lib` (STATIC) | **PASS** (0 warnings) | **SKIPPED (offline)** | Tests unconditionally call `FetchContent` for GTest. `sor --demo` verified cleanly. |
| 08 | `08-market-making-simulator` | `mmsim` (STATIC) | **PASS** (0 warnings) | **PASS (6/6, 3.48s)** | Zero external deps; all unit, fuzz, risk, exchange, and integration tests pass. |
| 09 | `09-latency-performance-lab` | `lpl` (`lpl::lpl`, STATIC) | **PASS** (0 warnings) | **PASS (4/4, 2.24s)** | Zero external deps (`LPL_BUILD_GBENCH=OFF` default). `quick_demo` A/B checksum match YES (`2.06x` p50 speedup `pooled_flat` vs `baseline_std_map`). |
| 10 | `10-cross-exchange-arbitrage` | `xarb_core` (STATIC) | **PASS** (0 warnings) | **PASS (5/5, 3.42s)** | Zero external deps (+ optional SQLite3 found & enabled). `xarb replay` invariants PASS. |

### Frozen Baseline Demo Checksums (SHA-256)

| Module | Command | SHA-256 / Digest |
|---|---|---|
| 01 (`pool` & `map`) | `me_cli replay --in examples/sample_session.txt --text-in --out - --text --book {pool,map} --symbols 2 --tick 5 --lot 10` | `24eb1bd9c696f15c0a38e81d9173eef491446695ec0f6b88e94793b33ae4d1bb` (matches `sample_session.expected.txt` byte-for-byte) |
| 02 | `itch_gen --messages 5000 --seed 42` -> `itch_replay` (`summary.csv` / `counts.csv`) | Book digest `a154d7340bc9d167`; `summary.csv` `b3ddd3a179a27a49cbaf3c015cef815853dba56e20e721736772cf1242ebb762`; `counts.csv` `fd3d04b9a967820b177abeb990c44d6f07bc59ca64dd68f96e3123ba79bd69d9` |
| 07 | `sor --demo --audit-out ...` | stdout `a74229b59072309d544b683a241957194fea848ff7ca8a6a3a09bfd5b4f8c8a3`; audit JSONL `52f561b58fcb653cc0d21b416c574c197d95cde23a2e60e44f386201d72fc870` |
| 08 | `mmsim generate --regime mean_reverting --seed 42 --duration 10 --rate 50` | `00fd4eba5645a2c94b696a1d2be0ed3244b309ec2d8a59f8fdd34c7450f7386f` (762 events) |
| 10 | `xarb replay --feed examples/sample_feed.xarb --scenario baseline` | Run hash `6fdc15e41a9fb447`; `summary.json` `cad5dc0bcd94708f45c01eb627fdd68e66d50fd01bb4a2db195b5a5859d295e4` |

---

## 3. API, Type Scale & Collision Inventory

### Namespaces & Core Scales

| Module | Namespace(s) | Price Type & Scale | Qty Type | Timestamp | Side | Symbol / Venue |
|---|---|---|---|---|---|---|
| 01 | `me` | `int64_t` (multiple of `tick_size`) | `int64_t` (multiple of `lot_size`) | None (`Seq` only) | `Buy=0, Sell=1` | `SymbolId = uint32_t` (0-based) |
| 02 | `itch` | `Price4 = uint32_t` ($10^{-4}$ USD) | `uint32_t` (msg) / `uint64_t` (book) | `uint64_t` ns since midnight | `Buy='B', Sell='S'` | `uint16_t stock_locate` (1-based in synth) + `char[8]` |
| 03 | `ouch`, `ouch::wire`, `ouch::soup`, `ouch::net`, `ouch::sim` | `uint32_t` ($10^{-4}$ USD; `0x7FFFFFFF` = Market) | `uint32_t` (`< 1,000,000`) | Wire: ns since midnight; OMS/Log: `steady_clock` ns | `char` (`'B'/'S'/'T'/'E'`) | `Symbol = Alpha<8>`, `Token = Alpha<14>` |
| 04 | `hftbt` | `int64_t` ticks (`tick_value` USD) | `int64_t` | `int64_t` sim ns | `Buy=0, Sell=1` | `InstrumentId = uint32_t` (0-based) |
| 05 | `lfm` | Generic `T` | Generic `T` | N/A | N/A | N/A |
| 06 | `mdf` | `int64_t` ticks | `uint32_t` / `uint64_t` | `uint64_t` `steady_clock` ns | `kBid=0, kAsk=1` | `uint16_t channel_id`, `uint32_t instrument_id` |
| 07 | `sor` | `int64_t` (`kPriceScale=10,000`, $10^{-4}$ USD) | `int64_t` | `int64_t` ns | `Buy=0, Sell=1` | `VenueId = uint8_t` (max 8); single-symbol state per instance |
| 08 | `mmsim` | `int64_t` ticks (`tick_size_dollars`, default `$0.01`) | `int64_t` | `int64_t` ns | `Bid=0, Ask=1` | Single-instrument, single-venue |
| 09 | `lpl` | `int64_t` ticks | `uint32_t` / `uint64_t` | `uint64_t` `steady_clock` ns | `kBuy=0, kSell=1` | Benchmark harness & micro-kernels |
| 10 | `xarb` | `int64_t` (`10^-price_decimals`) | `int64_t` (`10^-qty_decimals`) | `int64_t` ns | `Bid=0, Ask=1` | Fixed 3-venue enum (`Binance, Coinbase, Okx`) |

### Identified Collisions & Competing Duplicate Implementations

1. **CMake target name collisions (if using naive `add_subdirectory`):**
   - `test_order_book` exists in projects `06`, `08`, and `10`.
   - `test_wire` exists in projects `03` and `06`.
   - `test_concurrency` exists in projects `09` and `10`.
   - Projects `01`, `03`, `04`, `07` each declare `FetchContent` `googletest`; `01` and `05` declare `googlebenchmark`.
2. **Global macros / link traps:**
   - `01`: `me_alloc_counter` overrides global `operator new`/`delete` — must **never** be linked into the integrated engine.
   - `02`: `ITCH_FOR_EACH_MESSAGE(X)` remains defined in `itch/messages.hpp` (namespaced with `ITCH_` prefix, no conflict).
   - `09`: `lpl`'s `src/environment.cpp` expects private compile definitions (`LPL_BUILD_TYPE_STR`, `LPL_CXX_COMPILER_ID_STR`, `LPL_CXX_COMPILER_VER_STR`, `LPL_CXX_FLAGS_STR`, `LPL_MARCH_NATIVE_ENABLED`).
   - `10`: defines `XARB_HAS_SQLITE3=1` when SQLite3 is linked.
3. **Competing domain implementations (must select ONE authority per role):**
   - **Matching cores:** `01` (`me::Engine<PoolBook/MapBook>`), `03` (`ouch::sim::SimpleMatchingEngine`), `04` (`hftbt::Exchange`), `08` (`mmsim::SimulatedExchange`), `09` (`lpl::PooledFlatMatchingEngine`).
   - **Order books:** `01`, `02`, `04`, `06`, `07`, `08`, `09`, `10` each define a book.
   - **OMS & Risk:** `03` (`ouch::OrderManager` / `RiskEngine`), `04` (`hftbt::Oms`), `07` (`sor::OrderManagementSystem` / `RiskManager`), `08` (`mmsim::RiskManager`).
   - **SPSC queues:** `05` (`lfm::SpscQueue`), `09` (`lpl::SpscRingBuffer`), `10` (`xarb::SpscQueue`).
4. **Protocol & semantic subtleties caught during audit:**
   - **Cancel/Reduce semantics differ:** `01` (`Reduce`) and `03` (`CancelOrder::shares`) take *new remaining quantity*, whereas `02` (`OrderCancel::cancelled_shares`) takes a *decrement delta*.
   - **Limit vs Market/IOC:** `01` `me::Engine` supports Limit GTC only; Market and IOC semantics must be synthesized cleanly in the thin matching adapter (`New` + immediate `Cancel` of unfilled `leaves`).
   - **Symbol indexing:** `02` synthetic ITCH assigns 1-based `stock_locate` (`1..N`), whereas `01` and `04` use 0-based `SymbolId`/`InstrumentId`.
   - **SOR single-symbol state:** `07` `sor::ConsolidatedBook` / `OrderManagementSystem` do not key by `symbol_id`; multi-instrument routing instantiates one SOR stack per instrument.
   - **RiskEngine default:** `03` `ouch::RiskLimits::require_reference_price` defaults to `true`; adapter must explicitly feed observed reference prices.
   - **macOS platform behavior:** `pthread_setaffinity_np`, `recvmmsg`, `SO_BUSY_POLL`, and Linux `perf_event_open` are unavailable on macOS; `05` and `09` fall back cleanly to QoS hints and `proc_pid_rusage(RUSAGE_INFO_V4)`.

---

## 4. Open Architecture Decisions (Resolved for Integration)

| ADR | Decision | Rationale |
|---|---|---|
| **ADR-001** | **Authoritative Matching Engine:** Project `01` (`me::Engine<me::PoolBook>`, with `me::MapBook` selectable for differential checks and A/B benchmarks). | Cleanest price-time priority semantics, preallocated zero-hot-path-alloc pool book, and matches Project `03`'s `ouch::sim::MatchingEngine` virtual seam. |
| **ADR-002** | **Authoritative Event Scheduler & Clock Domains:** Dedicated `hft::runtime::DeterministicScheduler` ordered by `(event_time_ns, priority_class, source_sequence, insertion_sequence)`, using Project `04`'s `hftbt::LatencyPath` for FIFO monotonic delays. | Avoids dual event loops; strong types `SimTimeNs` vs `WallTimeNs` statically prevent mixing simulation time and wall-clock time. |
| **ADR-003** | **Authoritative OMS, Pre-Trade Risk & Accounting Ledger:** Unified `engine/` `PreTradeRisk`, `OmsReconciler` (bridging Project `03`'s `ouch::OrderManager` and `ouch::EventLog` journal), and fixed-point integer `AccountingLedger` (`Money` at $10^{-4}$ scale). | Eliminates floating-point drift on accounting paths, ensures worst-case exposure retains unacknowledged/uncertain orders, and deduplicates fills by `ExecId`. |
| **ADR-004** | **Explicit Protocol & ID Boundaries:** Separate adapters for ITCH 5.0 (`02`), MDF1/MDR1 (`06`), and OUCH/SoupBinTCP (`03`), with distinct `FeedOrderRef`, `ClientToken`, `ExchangeOrderId`, `ParentOrderId`, `ChildOrderId`, and `ExecId` types. | Prevents wire assumptions and ID collisions across market data, multicast recovery, routing, and execution layers. |
| **ADR-005** | **Monorepo Build Isolation:** `engine/cmake/UpstreamModules.cmake` compiles upstream library sources directly into isolated `hft_upstream_*` static targets without touching `projects/*/CMakeLists.txt`. | Zero target collisions, zero FetchContent leakage into `engine/`, and 100% preservation of independent project builds. |
