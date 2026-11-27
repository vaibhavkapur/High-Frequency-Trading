# Acceptance Report — FPGA Order Book Accelerator

**Schema Version:** `fpga-ob.v1`  
**Verification Date:** `2026-11-27`  
**Host Environment:** `macOS 26.5.1 (Apple M5, arm64)`, `Apple clang 21.0.0`, `CMake 4.5.1`, `Python 3.9.6` (see [`environment.json`](environment.json))

---

## 1. Project-Specific Acceptance Criteria

| Acceptance Criterion (`plan.md` §5) | Status | Measured Evidence / Raw Artifact |
|---|---|---|
| Zero mismatches in `>= 10,000` seeded legal events | **PASS** | `0` mismatches across **25,000** seeded legal 1-lane events, **25,000** seeded legal 2-lane events, and **10,000** mixed adversarial events (`seed = 20261127`). Raw output: [`differential_10k.json`](differential_10k.json). |
| Report cycles per event and tested clock assumptions | **PASS** | Measured on identical 100,000-event seeded stream (`warmup = 2,000`): Baseline FSM = `3.0000` cycles/event (`300,000` cycles); Pipelined 1-lane = `1.00003` cycles/event (`100,003` cycles, `3` cycles latency); Pipelined 2-lane = `0.50003` cycles/event (`50,003` cycles, `3` cycles latency). Analytical projections documented for `250 MHz` (`4.0 ns`) and `300 MHz` (`3.333 ns`). Raw output: [`benchmark_latency.json`](benchmark_latency.json). |
| Actual hardware timing only if measured | **PASS (Explicitly NOT RUN)** | `physical_hardware_timing_measured: false` in [`benchmark_latency.json`](benchmark_latency.json) and `NOT_RUN (no FPGA target hardware attached)` in [`environment.json`](environment.json). No fabricated silicon Fmax or LUT/FF counts are claimed. |
| All required test scenarios pass in supported environment; unavailable dependencies explicitly marked not run | **PASS** | CTest 4/4 passed (`unit_tests`, `integration_tests`, `differential_tests`, `python_reference_cross_check`). Optional EDA tools (`verilator`, `cocotb`, `yosys`) are not installed on this host and are explicitly marked `NOT_RUN` in [`environment.json`](environment.json). |
| README documents setup, demo, validation, assumptions, limitations; distinguishes actual observations vs estimates/targets | **PASS** | Documented in [`README.md`](../README.md), [`docs/contracts.md`](../docs/contracts.md), [`docs/architecture.md`](../docs/architecture.md), and [`docs/runbook.md`](../docs/runbook.md). |
| No real transactions, customer data, credentials, or proprietary/SAAT artifacts | **PASS** | 100% synthetic generated/deterministic fixtures and original source code. |

---

## 2. Test and Validation Matrix Mapping (`plan.md` §4)

| Test Area | Required Scenario | Implementation | Result & Evidence |
|---|---|---|---|
| **Domain correctness** | Reset mid-stream, invalid cancel, full capacity | `tests/test_unit.cpp` (`test_reset_mid_stream`, `test_invalid_cancel_exhaustive`, `test_full_capacity_and_order_overflow`, `test_deterministic_fixture`, `test_invalid_and_boundary_fixture`) | **PASS** — 7 unit test suites, 0 failures; verified against independent `ReferenceOrderBook` and `tests/fixtures/deterministic_basic.expected.json`. |
| **Integration and resilience** | Backpressure and simultaneous updates | `tests/test_integration.cpp` (`test_backpressure_patterns`, `test_simultaneous_updates`) | **PASS** — 2,500-event backpressure test (`1,392` stalls, `0` mismatches) + 7-pair simultaneous dual-lane & same-price collision test (`7` dual-lane cycles, `2` stalls, `0` mismatches). Raw output: [`scenarios.json`](scenarios.json). |
| **Boundary and failure behavior** | Randomized reference equivalence | `tests/test_differential.cpp` | **PASS** — 0 mismatches across 25,000 legal 1-lane + 25,000 legal 2-lane + 10,000 mixed adversarial (invalid schema/flags/opcode/side/OOB/zero-qty/overflow/capacity/invalid-cancel/backpressure/resets). Raw output: [`differential_10k.json`](differential_10k.json). |
| **Reproducibility** | Same versioned inputs/configuration and fixed seed | `tests/test_integration.cpp` (`test_binary_replay_reproducibility`), `scripts/reference_model.py`, `scripts/run_all_validations.py` | **PASS** — Binary 20-byte wire round-trip and repeated replay produce bit-identical BBO snapshots and diagnostic counters; independent Python reference matches `deterministic_basic.expected.json`. |
| **Operational review** | Missing configuration, unsupported inputs, restart/rerun | `src/cli_main.cpp`, `scripts/run_all_validations.py`, `docs/runbook.md` | **PASS** — Missing fixture exits with code `2` and explicit JSON error (`CONFIG_MISSING_OR_INVALID`); unsupported schema/flags/opcodes return explicit `ERR_UNSUPPORTED_*` codes without mutating book state. |

---

## 3. Capabilities Explicitly Marked NOT RUN in Local Environment

| Capability | Artifact Ready? | Local Host Status | How to Execute |
|---|---|---|---|
| SystemVerilog RTL compilation & simulation (`rtl/*.sv`) | Yes (`sim/verilator_main.cpp`, `Makefile`) | **NOT RUN** (`verilator` not installed) | `brew install verilator && make sim-verilator` or GitHub Actions CI |
| cocotb Python RTL coroutine testbench | Yes (`tests/cocotb/test_order_book.py`, `Makefile`) | **NOT RUN** (`verilator` / `cocotb` not installed) | `pip install -r requirements.txt && make sim-cocotb` or GitHub Actions CI |
| Yosys RTL synthesis resource estimation | Yes (`scripts/synth_yosys.ys`, `Makefile`) | **NOT RUN** (`yosys` not installed) | `brew install yosys && make synth-yosys` or GitHub Actions CI |
| Physical FPGA place-and-route / silicon timing | Out of scope (no target board) | **NOT RUN** (no FPGA hardware attached) | Requires vendor toolchain (Vivado/Quartus) and target FPGA part |

---

## 4. Raw Machine-Readable Artifacts

- [`reports/environment.json`](environment.json) — Host OS, CPU architecture, compiler/tool versions, operational CLI check output, optional EDA run status.
- [`reports/demo_output.json`](demo_output.json) — Captured deterministic 12-event demo fixture output with per-event BBO snapshots and diagnostics.
- [`reports/scenarios.json`](scenarios.json) — Integration scenario metrics (backpressure, simultaneous dual-lane collisions, binary replay reproducibility).
- [`reports/differential_10k.json`](differential_10k.json) — 25,000-event legal (1-lane & 2-lane) and 10,000-event mixed adversarial differential verification results.
- [`reports/benchmark_latency.json`](benchmark_latency.json) — 100,000-event like-for-like cycle-latency and host simulation comparison across baseline FSM vs pipelined 1-lane vs pipelined 2-lane variants.
