# Inventory-Aware Market-Making Strategy Simulator

> An order-book-based market-maker with explicit fill, inventory and adverse-selection models.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20 strategy engine, Python/pandas for analytics, matplotlib, pytest  
**Status:** Implemented with verified unit/fuzz/integration tests, ASan/UBSan clean builds, latency benchmarks, and 12-session Train/Validation/Test out-of-sample experiments.

## Why build this?

Implement a conservative market-making research sandbox using an event-driven simulated exchange and OMS—not a promise of profitable automated trading.

**Hiring signal:** Demonstrates systems and microstructure design tradeoffs, production-style correctness tests (causality, cancel races, accounting reconciliation, binary fuzzing), and reproducible empirical measurements.

## Architecture

```text
.mmbin / .csv replay -> exchange L2 book -> simulated OMS (queue-priority / touch fills, cancel races)
        |                                                               |
        | + md_latency_ns                                               | + ack_latency_ns
        v                                                               v
strategy L2 book -> book signals (ToB, microprice, OBI, OFI, vol) -> fills / P&L / markouts (10ms, 100ms, 1s)
                         |
                         v
              fair-value & inventory-aware quoting -> pre-trade risk limits -> (+ order_latency_ns) -> exchange
```

See [docs/design.md](docs/design.md) for detailed component design and [docs/assumptions.md](docs/assumptions.md) for explicit execution-model assumptions and limitations.

## Implemented Features

- **L2 Order Books & Wire Protocol**: Dual L2 order books (`FlatArrayBook` accelerated by a 2-level C++20 `<bit>` 64-bit occupancy bitset, and reference `MapBook` for differential testing), plus a CRC32-checked binary replay format (`.mmbin`) and CSV feed support.
- **Strictly Causal Book Signals**: Top-of-book, microprice, L1 and depth-weighted top-K order-book imbalance (`OBI`), Cont-Kukanov-Stoikov short-horizon exponentially weighted order-flow imbalance (`OFI`), and EWMA realized volatility (`ticks / sqrt(s)`).
- **Inventory-Aware Quoting & Baseline**:
  - `InventoryAwareStrategy`: `reservation_price = fair_value - inventory_penalty(q, sigma)`, volatility- and fee-adjusted half-spread, inventory-aware size throttling, and quote-refresh hysteresis.
  - `NaiveSymmetricStrategy`: fixed-spread symmetric quoting around mid with zero skew, evaluated under identical latency, fees, and pre-trade risk controls.
- **Pre-Trade Risk Controls**: Worst-case exposure checks across confirmed position + all `PendingNew`, `Live`, and `PendingCancel` orders for `max_abs_position`, `max_order_size`, `max_gross_notional_dollars`, `max_open_orders`, and a `max_drawdown_dollars` kill-switch that immediately cancels all resting quotes.
- **Explicit Fill, Latency & Fee Mechanics**: Separate `md_latency_ns`, `order_latency_ns`, and `ack_latency_ns`; cancel-race semantics (fills arriving before an in-flight cancel execute and return `TooLateToCancel`); explicit `queue_priority` and `probabilistic_touch` fill models; maker/taker bps and cancel fees.
- **Accounting & Adverse-Selection Analytics**: Separate realized P&L, unrealized P&L, gross MTM, fees, net MTM, and cash reconciliation; markout decomposition (`captured_half_spread = realized_spread + adverse_selection`) at `10 ms`, `100 ms`, and `1 s` strictly where session timestamps support each horizon.

## Repository Structure

```text
08-market-making-simulator/
├── README.md
├── CMakeLists.txt
├── requirements.txt
├── .clang-format
├── .github/workflows/ci.yml
├── include/mmsim/                 # Public C++20 headers
├── src/                           # Engine, books, protocol, exchange, risk, strategies, CLI
├── tests/                         # Unit, differential, fuzz, causality, and integration tests
├── benchmarks/                    # Reproducible throughput and latency benchmark harness
├── examples/                      # Strategy configs and tiny CSV replay
├── python/                        # Pandas accounting audit, matplotlib figures, pytest suite
├── scripts/                       # demo.sh, run_benchmarks.sh, run_experiments.py, format.sh
├── docs/                          # design.md, assumptions.md, research_report.md, sample_output.txt
└── results/                       # Raw benchmark outputs, 12-session CSVs/JSONs, figures
```

## Benchmarks and Measured Systems Tradeoff

### Environment and Headline Measurements

Raw files: [results/benchmarks/bench_results.csv](results/benchmarks/bench_results.csv), [results/benchmarks/environment.txt](results/benchmarks/environment.txt), [results/benchmarks/bench_stdout.txt](results/benchmarks/bench_stdout.txt).

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (10 physical / 10 logical cores, arm64, 16 GiB RAM) |
| OS / kernel | macOS Darwin 25.5.0 (`arm64`) |
| Compiler / version / flags | Apple Clang 21.0.0 (`-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`) |
| Dataset / event mix / sample count | 154,223 synthetic `high_vol_burst` L2 BookDelta + Trade events (120 s @ 1 kHz, seed `202601`) |
| Warmup / runs | 2 full warmup replays + 7 measured replays; batched (32 events) hot-path latency sampling |
| Throughput (`flat_array + inventory_aware`) | **6.58 M events/s** (full event-driven replay including exchange, OMS, risk, strategy) |
| Hot-path latency (`flat_array + inventory_aware`) | **p50 = 35.2 ns / p99 = 43.0 ns / p99.9 = 46.9 ns** (mean 34.9 ns) |
| Reproduction command | `./scripts/run_benchmarks.sh` |

### Measured Engineering Tradeoff: Order-Book Representation & Occupancy Bitset

An initial linear-scan `FlatArrayBook` (4,096-tick window) was **slower** than `std::map` (`2.46 M/s`, `205.7 ns` p50) because `top_levels(5)` scanned empty slots on sparse books. Adding a 2-level 64-bit occupancy bitset (`std::countl_zero` / `std::countr_zero`) eliminated empty-slot scans while keeping zero hot-path allocations:

| Configuration | Throughput (full replay) | Hot-Path Mean (ns) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|
| `flat_array` (2-level bitset) + `inventory_aware` | **6.58 M/s** | 34.9 | **35.2** | **43.0** | **46.9** |
| `std_map` + `inventory_aware` | 5.25 M/s | 46.0 | 44.2 | 63.8 | 149.6 |
| `flat_array` (2-level bitset) + `naive_symmetric` | **8.75 M/s** | 34.2 | **33.8** | **37.8** | **48.7** |
| `std_map` + `naive_symmetric` | 6.76 M/s | 44.6 | 44.2 | 57.3 | 65.3 |

`FlatArrayBook` (with bitset) achieves **+25% higher full-replay throughput** and **3.2x lower p99.9 tail latency** than `std::map` while producing bit-identical simulation results (`test_integration::FlatArrayAndStdMapProduceBitIdenticalSimulation`).

## Out-of-Sample Research Summary (12 Chronological Sessions)

Full methodology, robustness sweeps, and plots are in [docs/research_report.md](docs/research_report.md) and [results/experiments/sessions_summary.csv](results/experiments/sessions_summary.csv). Parameters (`risk_aversion=0.6`, `vol_spread_mult=0.25`) were selected strictly on Train (`S01`–`S04`), verified on Validation (`S05`–`S08`), and evaluated untouched on Test (`S09`–`S12`) under a positive `+0.20 bps` maker fee and `100/150/100 us` latencies:

| Test Session | Regime | Strategy | Fills | Gross MTM ($) | Fees ($) | Net MTM ($) | Max DD ($) | RMS Pos (sh) | AdvSel 1s (ticks) |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| S09 | `mean_reverting` | `naive_symmetric` | 2,695 | +2,502.05 | 466.10 | +2,035.95 | 17.32 | 275.3 | +0.040 |
| S09 | `mean_reverting` | `inventory_aware` | 990 | +676.05 | 171.01 | +505.04 | 17.95 | 155.4 | -0.088 |
| S10 | `toxic_trend` | `naive_symmetric` | 1,638 | -1,822.90 | 312.41 | **-2,135.31** | **2,142.11** (kill) | 298.3 | +0.699 |
| S10 | `toxic_trend` | `inventory_aware` | 752 | +1,722.13 | 135.07 | **+1,587.06** | **20.03** | 155.9 | -0.561 |
| S11 | `high_vol_burst` | `naive_symmetric` | 3,933 | +3,048.10 | 785.68 | +2,262.42 | 148.80 | 291.0 | +0.512 |
| S11 | `high_vol_burst` | `inventory_aware` | 1,595 | +1,711.40 | 272.49 | +1,438.91 | 32.34 | 162.0 | -0.337 |
| S12 | `thin_book` | `naive_symmetric` | 4,179 | +4,758.50 | 827.67 | +3,930.83 | 25.00 | 275.9 | +0.158 |
| S12 | `thin_book` | `inventory_aware` | 1,568 | +1,348.90 | 265.93 | +1,082.97 | 24.03 | 160.8 | -0.169 |

- **Honest tradeoff**: In calm mean-reverting sessions (`S09`, `S12`), `inventory_aware` captures less volume and lower dollar P&L than tight symmetric quoting, while cutting mean max drawdown across Test sessions by **24.7x** (`$23.59` vs `$583.31`), cutting RMS inventory by **44%** (`158.5 sh` vs `285.1 sh`), and avoiding the toxic-trend blowup (`-$2,135.31` on `S10`).
- **Robustness**: Under `20 ms` one-way latency (`S07`), `naive_symmetric` turns negative (`-$424.04`, 466 cancel races) while `inventory_aware` degrades to `+$53.15` (288 cancel races). Under `+1.00 bps` maker fees, `naive_symmetric` loses `-$979.69` while `inventory_aware` stays positive at `+$871.26`.

## Correctness and Failure-Mode Verification

All 6 C++ test suites pass under both Release (`-Werror`) and Debug ASan+UBSan (`-DMMSIM_SANITIZE=ON`), and all 5 Python `pytest` checks pass:

- `test_protocol_fuzz`: Binary header/record roundtrip, exhaustive 320-bit single-bit-flip CRC checks, 50,000 random-buffer fuzz iterations, out-of-order timestamp rejection.
- `test_order_book`: 50,000-op randomized differential check (`FlatArrayBook` vs `MapBook`) including lossless price-jump recentering.
- `test_features`: Exact microprice, L1/top-K imbalance, OFI, realized volatility, and inventory skew sign verification.
- `test_risk_and_pnl`: Worst-case position/notional/order-size/drawdown checks, 2,000-fill randomized accounting reconciliation (`max error <= 1.06e-11` dollars across all 24 experimental runs), and strict markout timestamp-support filtering.
- `test_exchange`: Queue-priority depletion, cancel-race semantics (fills arriving before in-flight cancels are never erased), post-only rejection, and wider-spread fill monotonicity under both fill models.
- `test_integration`: Strict causality / zero-lookahead mutation test, inventory bounds across all 4 regimes, end-to-end cancel race with non-zero latency, and `FlatArrayBook` vs `MapBook` bit-identical simulation check.

## Local Development & Reproduction

```bash
# 1. Build Release with strict warnings and run C++ unit/fuzz/integration tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMMSIM_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# 2. Build and run with AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMMSIM_SANITIZE=ON -DMMSIM_WERROR=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

# 3. Run end-to-end terminal demo (sample output saved in docs/sample_output.txt)
./scripts/demo.sh

# 4. Run reproducible latency & throughput benchmarks
./scripts/run_benchmarks.sh

# 5. Run 12-session Train/Val/Test study, robustness sweeps, figures, and pytest
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 scripts/run_experiments.py
pytest -v python/test_analytics.py
```

## Definition of Done

- [x] Working implementation with a documented supported feature/protocol subset ([docs/design.md](docs/design.md))
- [x] Unit, integration, negative-case, fuzz, causality, and replay/stress tests (6 C++ suites + ASan/UBSan + 5 pytest checks)
- [x] Reproducible benchmark script and machine/environment metadata ([scripts/run_benchmarks.sh](scripts/run_benchmarks.sh), [results/benchmarks/environment.txt](results/benchmarks/environment.txt))
- [x] Measured performance tradeoff with raw results (`FlatArrayBook` bitset vs `MapBook`, [results/benchmarks/bench_results.csv](results/benchmarks/bench_results.csv))
- [x] Architecture diagram and explicit assumptions/limitations ([docs/design.md](docs/design.md), [docs/assumptions.md](docs/assumptions.md))
- [x] Terminal demo and captured sample output ([scripts/demo.sh](scripts/demo.sh), [docs/sample_output.txt](docs/sample_output.txt))
- [x] CI workflow that builds (Release + ASan/UBSan + `-Werror`), runs CTest/pytest, and checks formatting ([.github/workflows/ci.yml](.github/workflows/ci.yml))

## CV Bullet

> Built an inventory-aware market-making simulator in C++20 and Python using order-book imbalance and microprice signals, latency-aware quotes, bitset-accelerated L2 books (6.58 M events/s, 35.2 ns p50), and worst-case pre-trade controls; reported out-of-sample net P&L, adverse selection, and inventory risk across **12** sessions.

## Caveats / Non-Goals

- Sharpe ratios are not reported on short synthetic sessions without multi-day sampling and annualization assumptions.
- Resting quotes are not assumed to fill on every touch; both `queue_priority` and `probabilistic_touch` are explicitly labeled modeled execution assumptions rather than exchange L3 queue telemetry.
- This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further Reading

- *Trading and Exchanges* — Larry Harris (market structure)
- *Algorithmic and High-Frequency Trading* — Cartea, Jaimungal & Penalva (inventory-aware market making)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
