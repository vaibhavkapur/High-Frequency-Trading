# FPGA Order Book Accelerator

Deterministic hardware prototype for processing a bounded synthetic L2 order-book event stream (`ADD`, `CANCEL`, `EXECUTE`, `NOP`), extracting Best Bid / Best Offer (BBO) via a pipelined parallel binary reduction tree, and verifying bit-exact equivalence against independent software reference models.

> **Scope & Safety Notice:** Independently developed educational and research prototype using synthetic fixtures only. RTL/cycle-accurate simulation is **not** physical silicon latency evidence. No live capital, exchange gateways, real customer data, or credentials are included.

---

## Architecture Overview

- **Synthesizable SystemVerilog RTL (`rtl/`):**
  - [`rtl/order_book_pkg.sv`](rtl/order_book_pkg.sv) — 160-bit fixed-width `event_pkt_t`, `bbo_pkt_t`, error status codes, and diagnostic counters (`fpga-ob.v1`).
  - [`rtl/price_level_bank.sv`](rtl/price_level_bank.sv) — Direct-indexed bounded price-level bank (`NUM_LEVELS = 64`, `MAX_ACTIVE_LEVELS = 32`) with dual-lane atomic combinational forwarding for simultaneous updates.
  - [`rtl/bbo_tree.sv`](rtl/bbo_tree.sv) — Parameterized `O(log2(NUM_LEVELS))` binary reduction priority tree for Best Bid (highest active) and Best Ask (lowest active).
  - [`rtl/order_book_top.sv`](rtl/order_book_top.sv) — 3-stage synchronous pipeline (`Decode -> Atomic Bank Update -> BBO Tree & Output Reg`) with global AXI-Stream backpressure (`in_valid`/`in_ready`, `out_valid`/`out_ready`), synchronous active-low reset, and diagnostic counters.
- **Cycle-Accurate C++20 Model & Independent References (`include/`, `src/`, `scripts/`):**
  - `fpga_ob::PipelinedOrderBookAccelerator` — Cycle-accurate C++20 mirror of `rtl/order_book_top.sv` (`Latency = 3` cycles, `II = 1.0` single-lane / `0.5` dual-lane).
  - `fpga_ob::BaselineFsmOrderBook` — Unpipelined 3-state FSM baseline (`II = 3.0` cycles/event) for like-for-like benchmark comparison.
  - `fpga_ob::ReferenceOrderBook` (C++ `std::map`) and [`scripts/reference_model.py`](scripts/reference_model.py) (Python `dict`) — Completely independent golden references.
- **Optional EDA Harnesses (`sim/`, `tests/cocotb/`, `scripts/synth_yosys.ys`):**
  - Ready-to-run Verilator C++ differential harness, cocotb coroutine testbench, Yosys synthesis script, and GitHub Actions CI workflow (`.github/workflows/ci.yml`).

---

## Quickstart & Setup

### Prerequisites
- C++20 compiler (`clang++` or `g++`), CMake `>= 3.20`, `make`, Python `>= 3.9` (standard library only).
- Optional for native RTL simulation/synthesis: `verilator`, `yosys`, and `pip install -r requirements.txt` (`cocotb`, `pytest`).

### Build and Run Demo

```bash
cd projects/13-fpga-order-book-accelerator
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run deterministic 12-event demo fixture
./build/fpga_ob_cli --mode demo --fixture tests/fixtures/deterministic_basic.json
```

#### Example Demo Output (Actual Observation from `reports/demo_output.json`)

```json
{
  "schema_version": "fpga-ob.v1",
  "mode": "demo",
  "fixture": "deterministic_basic_v1",
  "events_processed": 12,
  "reference_mismatches": 0,
  "diagnostics": {
    "cycles_total": 16,
    "reset_cycles": 1,
    "events_accepted": 12,
    "events_committed": 12,
    "events_rejected": 0,
    "backpressure_stalls": 0,
    "dual_lane_cycles": 0,
    "max_active_bid_levels": 2,
    "max_active_ask_levels": 2
  },
  "final_bbo": {
    "status_0": "STATUS_OK",
    "seq_no_0": 12,
    "best_bid_valid": true,
    "best_bid_price": 10010,
    "best_bid_qty": 100,
    "best_bid_orders": 1,
    "best_ask_valid": true,
    "best_ask_price": 10015,
    "best_ask_qty": 200,
    "best_ask_orders": 1,
    "spread": 5,
    "crossed": false,
    "active_bid_levels": 1,
    "active_ask_levels": 1
  }
}
```

### Run Full Validation & Benchmarks

```bash
# Runs build, CTest (unit + integration + 25k differential + Python ref check),
# benchmarks, operational failure checks, and writes raw JSON reports to reports/
python3 scripts/run_all_validations.py

# Optional EDA targets (explicitly report SKIPPED if tools are not installed)
make sim-verilator
make sim-cocotb
make synth-yosys
```

---

## Measured Results vs. Analytical Projections

Measured locally on **macOS 26.5.1 (Apple M5, arm64)**, **Apple clang 21.0.0**, **Release build** (`-O3`), fixed seed `20261127`, `warmup = 2,000`, `measured = 100,000` identical seeded legal events (`NUM_LEVELS = 64`, `MAX_ACTIVE_LEVELS = 32`). Raw data: [`reports/benchmark_latency.json`](reports/benchmark_latency.json) and [`reports/differential_10k.json`](reports/differential_10k.json).

### Cycle-Accurate Simulation Benchmark (Actual Observations vs. Clock Projections)

| Variant | Simulated Events (Observed) | Total Cycles (Observed) | First-Event Latency (Observed Cycles) | Cycles / Event (Observed) | Projected Latency @ 250 MHz *(Estimate)* | Projected Throughput @ 250 MHz *(Estimate)* | Projected Latency @ 300 MHz *(Estimate)* |
|---|---:|---:|---:|---:|---:|---:|---:|
| `baseline_fsm_unpipelined` | 100,000 | 300,000 | 3 | `3.0000` | `12.0 ns` | `83.33 Meps` | `10.0 ns` |
| `pipelined_tree_1lane` | 100,000 | 100,003 | 3 | `1.0000` | `12.0 ns` | `249.99 Meps` | `10.0 ns` |
| `pipelined_tree_2lane` | 100,000 | 50,003 | 3 | `0.5000` | `12.0 ns` | `499.97 Meps` | `10.0 ns` |

### Differential Equivalence Summary (Actual Observations)

- **25,000 seeded legal 1-lane events (`seed=20261127`):** `0` mismatches (`25,003` cycles, `1.00012` cycles/event).
- **25,000 seeded legal 2-lane events (`seed=20261127`):** `0` mismatches (`12,503` cycles, `0.50012` cycles/event).
- **10,000 seeded mixed adversarial events** (invalid schema/flags/opcode/side/OOB/zero-qty/overflow/full-capacity/invalid-cancel + random backpressure + dual-lane + 3 mid-stream resets): `0` mismatches (`1,642` backpressure stalls).

---

## Assumptions and Known Limitations

1. **Simulated Cycles vs. Physical Timing:** `250 MHz` (`4.0 ns`) and `300 MHz` (`3.333 ns`) latency/throughput figures are strictly analytical projections from simulated clock cycles; **physical FPGA timing closure and silicon measurements were NOT RUN**.
2. **Optional EDA Tools on Local Host:** `verilator`, `cocotb`, and `yosys` are not installed in this local macOS environment and are explicitly recorded as `NOT_RUN` in [`reports/environment.json`](reports/environment.json). The SystemVerilog RTL, Verilator harness, cocotb testbench, Yosys script, and Linux CI workflow are included for execution on EDA-equipped hosts.
3. **Bounded L2 Price-Level Scope:** Supports contiguous bounded ticks `[PRICE_BASE, PRICE_BASE + NUM_LEVELS - 1]` and aggregate L2 quantities/order counts per level. Individual L3 order-ID tracking and automatic order matching are explicitly out of scope; crossed states are signaled via `crossed = true`.
4. **Tree Depth Scaling:** Single-cycle combinational `bbo_tree` targets `NUM_LEVELS <= 64` (6 logic levels). Larger books (`>= 512` levels) would require pipelining the reduction tree (adding 1 cycle of latency while preserving `II = 1`).

---

## Documentation & Evidence Index

- **Implementation Plan:** [`plan.md`](plan.md)
- **Data Contracts & Binary Interface:** [`docs/contracts.md`](docs/contracts.md)
- **Architecture & Pipeline Timing:** [`docs/architecture.md`](docs/architecture.md) and [`docs/adr/0001-bounded-pipelined-order-book-architecture.md`](docs/adr/0001-bounded-pipelined-order-book-architecture.md)
- **Integration & Dependencies:** [`docs/integration.md`](docs/integration.md)
- **Operator / Developer Runbook:** [`docs/runbook.md`](docs/runbook.md)
- **Acceptance Evidence Report:** [`reports/acceptance.md`](reports/acceptance.md)
- **Raw Reports:** [`reports/differential_10k.json`](reports/differential_10k.json), [`reports/scenarios.json`](reports/scenarios.json), [`reports/benchmark_latency.json`](reports/benchmark_latency.json), [`reports/demo_output.json`](reports/demo_output.json), [`reports/environment.json`](reports/environment.json)
