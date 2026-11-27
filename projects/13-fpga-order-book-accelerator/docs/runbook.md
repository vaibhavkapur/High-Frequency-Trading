# Operator and Developer Runbook

## 1. Prerequisites and Clean Setup

### Core C++20 + Python Environment (Required)
- C++20 compiler (`clang++` or `g++`)
- CMake `>= 3.20` and `make`
- Python `>= 3.9` (standard library only for core validation)

### Optional RTL Simulation & Synthesis Toolchain
To additionally run SystemVerilog compilation, cocotb tests, and Yosys synthesis estimates:
```bash
# macOS (Homebrew)
brew install verilator yosys
python3 -m pip install -r requirements.txt

# Ubuntu / Debian
sudo apt-get update && sudo apt-get install -y verilator yosys cmake g++ python3-pip
python3 -m pip install -r requirements.txt
```

## 2. Build and Demo Walkthrough

From `projects/13-fpga-order-book-accelerator/`:

```bash
# 1. Configure and build in Release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2. Run the deterministic demo fixture with structured JSON output
./build/fpga_ob_cli --mode demo --fixture tests/fixtures/deterministic_basic.json

# 3. Run replay from a binary stream with diagnostic counters
./build/fpga_ob_cli --mode replay --fixture tests/fixtures/invalid_and_boundary.json --verbose
```

## 3. Full Validation Suite

Run the end-to-end validation runner, which builds, executes all unit/integration/differential tests, runs the Python independent reference cross-check, runs the cycle-latency benchmark, checks optional EDA tools, and writes raw JSON outputs to `reports/`:

```bash
python3 scripts/run_all_validations.py
```

Or invoke individual stages via `make`:

```bash
make build          # Build C++20 library, CLI, tests, and benchmarks
make test           # Run CTest (unit, integration, 25k differential, Python ref check)
make bench          # Run baseline vs pipelined cycle-latency benchmarks
make sim-verilator  # Run Verilator RTL simulation (requires verilator)
make sim-cocotb     # Run cocotb RTL testbench (requires verilator + cocotb)
make synth-yosys    # Run Yosys synthesis resource estimate (requires yosys)
```

## 4. Diagnostics and Counters

Every simulation run reports structured counters (`Diagnostics` struct / JSON `diagnostics` object):
- `cycles_total`: Total clock edges elapsed (excluding reset cycles).
- `reset_cycles`: Total clock edges spent with `rst_n == 0`.
- `events_accepted`: Ingress lane events accepted (`in_valid && in_ready`).
- `events_committed`: Events exiting Stage 3 with `STATUS_OK`.
- `events_rejected`: Events exiting Stage 3 with non-zero error status.
- `backpressure_stalls`: Cycles where `in_valid == 1` while `in_ready == 0`.
- `dual_lane_cycles`: Cycles where both Lane 0 and Lane 1 were accepted simultaneously.
- Error breakdown: `err_unsupported_schema`, `err_unsupported_flags`, `err_unsupported_opcode`, `err_invalid_side`, `err_price_oob`, `err_zero_qty`, `err_full_capacity`, `err_overflow_qty`, `err_overflow_orders`, `err_invalid_cancel`, `err_invalid_execute`.
- High-water marks: `max_active_bid_levels`, `max_active_ask_levels`.

## 5. Failure Modes and Recovery Procedures

| Symptom / Failure | Diagnostic Output | Recovery Action |
|---|---|---|
| Missing fixture or config path (`--fixture missing.json`) | Non-zero exit code (`2`), explicit stderr JSON error `{"error":"CONFIG_MISSING", ...}` (never silent success). | Verify path relative to `projects/13-fpga-order-book-accelerator/` or pass absolute path. |
| Unsupported schema version / flags / opcode | Event rejected with `ERR_UNSUPPORTED_*`, book state unchanged, counter incremented, warning emitted in verbose mode. | Inspect `seq_no` in output stream; fix upstream producer to emit `schema_ver=1`, `flags=0`, valid opcode `0..3`. |
| `ERR_FULL_CAPACITY` (`active_level_count == MAX_ACTIVE_LEVELS`) | New inactive price level rejected without corrupting existing levels. | Increase `MAX_ACTIVE_LEVELS` compile/config parameter (up to `NUM_LEVELS`) or cancel stale far-from-touch levels before adding new levels. |
| Reset mid-stream / restart / rerun | Assert synchronous `rst_n = 0` for `>= 1` clock cycle (or re-run CLI with identical `--seed`). | Book, pipeline registers, and counters reset cleanly to deterministic zero state; replaying the exact same binary/JSON fixture produces bit-identical outputs. |
| Optional EDA tool (`verilator`, `cocotb`, `yosys`) missing | `Makefile` and `run_all_validations.py` explicitly log `NOT_RUN (tool not installed)` and record status in `reports/environment.json`. | Install optional EDA tools per Section 1 or run GitHub Actions CI workflow (`.github/workflows/ci.yml`). |
