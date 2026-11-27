# ADR-0001: Bounded Direct-Indexed 3-Stage Pipelined Order Book Architecture

- **Status:** Accepted
- **Date:** 2026-11-27
- **Project:** `projects/13-fpga-order-book-accelerator`

## Context

We need a deterministic, synthesizable hardware prototype that processes fixed-width synthetic order-book events (`ADD`, `CANCEL`, `EXECUTE`, `NOP`), maintains bounded price levels, extracts Best Bid and Best Offer (BBO) with deterministic cycle latency, handles downstream backpressure and simultaneous updates, and enables bit-exact differential verification against an independent software reference.

## Decision

1. **Direct-Indexed Price-Level Banks (`NUM_LEVELS = 64`, `MAX_ACTIVE_LEVELS = 32`):**  
   Map each price `P` in `[PRICE_BASE, PRICE_BASE + NUM_LEVELS - 1]` directly to index `P - PRICE_BASE` in separate `bid` and `ask` register banks. Enforce an explicit `MAX_ACTIVE_LEVELS` active-level capacity guard so full-capacity behavior is deterministic and testable even when `NUM_LEVELS > MAX_ACTIVE_LEVELS`.
2. **3-Stage Synchronous Pipeline (`Decode -> Atomic Bank Update -> BBO Tree & Output`):**  
   Commit bank mutations in Stage 2 and extract BBO via a combinational binary reduction tree (`bbo_tree`) in Stage 3. This eliminates RAW/WAW hazards across consecutive cycles (`II = 1`) and allows a clean single-signal pipeline stall (`pipe_advance = !out_valid || out_ready`) under backpressure.
3. **Dual-Lane Simultaneous Ingress (`Lane 0` + optional `Lane 1`):**  
   Evaluate `Lane 0` and `Lane 1` sequentially in combinational logic within Stage 2 before a single register writeback, guaranteeing bit-exact equivalence to sequential order even when both lanes target the same `(side, price)` in the same cycle.
4. **Dual Verification Path (SystemVerilog RTL + Cycle-Accurate C++20 Model + Independent Software Reference):**  
   Author synthesizable SystemVerilog RTL (`rtl/*.sv`) along with Verilator/cocotb harnesses and Yosys synthesis scripts, plus a standalone cycle-accurate C++20 model and independent C++/Python software references so full deterministic, integration, and `>= 10,000`-event differential testing runs reproducibly on hosts with or without EDA tools installed.

## Consequences

- **Positive:** Strictly deterministic `3`-cycle latency and `1.0` cycle/event (`0.5` dual-lane) throughput; zero hazard bubbles; exact backpressure and reset semantics; clean separation between hot-path state machine, I/O contracts, and verification harnesses.
- **Negative / Trade-offs:** Price range is bounded to `NUM_LEVELS` ticks; L3 per-order-ID matching is out of scope; hosts lacking Verilator/Yosys execute the cycle-accurate C++20 RTL model and explicitly mark RTL simulator and hardware synthesis runs as not executed.
