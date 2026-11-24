# Simulation Experiments & Measured Tradeoffs

All tables below are generated directly by `python3 scripts/run_experiments.py` (300 parent orders per run, `seed=42`, decoupled market/order/execution PRNG streams so every policy faces the exact same market tape). Raw outputs are saved in `results/experiments/`.

## 1. Controlled Scenario Comparison (Baseline vs. Fee-Aware vs. Smart Cost-Aware)

Source: [scenarios.csv](../results/experiments/scenarios.csv)

| Scenario | Policy | Fill % | Gross IS (bps) | Fee (bps) | Net IS (bps) | Cxl/Fill | Failovers | Med Lat (us) | Venue Shares |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| `fee_inversion` | `naive_primary` | 97.0% | 0.61 | 0.23 | 0.84 | 0.04 | 0 | 240.9 | ARCA:100%, BATS_INV:0%, IEX_SIM:0% |
| `fee_inversion` | `naive_best_price` | 100.0% | 0.34 | 0.12 | 0.47 | 0.05 | 0 | 247.4 | ARCA:58%, BATS_INV:15%, IEX_SIM:26% |
| `fee_inversion` | `fee_aware` | 100.0% | 0.38 | -0.02 | **0.36** | 0.04 | 0 | 248.0 | ARCA:20%, BATS_INV:51%, IEX_SIM:29% |
| `fee_inversion` | `smart_cost_aware` | 99.9% | 0.36 | 0.03 | **0.39** | 0.04 | 0 | 247.1 | ARCA:42%, BATS_INV:46%, IEX_SIM:13% |
| `stale_feed_trap` | `naive_primary` | 97.4% | 0.75 | 0.20 | 0.94 | 0.05 | 0 | 241.6 | ARCA:100%, BATS_INV:0%, IEX_SIM:0% |
| `stale_feed_trap` | `naive_best_price` | 99.1% | 0.37 | 0.10 | 0.46 | 0.25 | 0 | 890.6 | ARCA:55%, BATS_INV:23%, IEX_SIM:22% |
| `stale_feed_trap` | `fee_aware` | 98.6% | 0.36 | 0.06 | 0.42 | 0.11 | 0 | 899.2 | ARCA:30%, BATS_INV:24%, IEX_SIM:46% |
| `stale_feed_trap` | `smart_cost_aware` | **99.4%** | 0.42 | 0.09 | 0.51 | **0.08** | 0 | **467.5** | ARCA:55%, BATS_INV:27%, IEX_SIM:19% |
| `latency_race_depletion` | `naive_primary` | 96.9% | 0.70 | 0.07 | 0.76 | 0.03 | 0 | 161.3 | ARCA:100%, BATS_INV:0%, IEX_SIM:0% |
| `latency_race_depletion` | `naive_best_price` | 98.8% | 0.42 | 0.07 | 0.49 | 0.49 | 0 | 4514.1 | ARCA:42%, BATS_INV:51%, IEX_SIM:7% |
| `latency_race_depletion` | `fee_aware` | 98.8% | 0.42 | 0.07 | 0.49 | 0.49 | 0 | 4514.1 | ARCA:42%, BATS_INV:51%, IEX_SIM:7% |
| `latency_race_depletion` | `smart_cost_aware` | **99.6%** | 0.53 | 0.07 | 0.60 | **0.09** | 0 | **880.4** | ARCA:81%, BATS_INV:19%, IEX_SIM:0% |
| `venue_failover` | `naive_primary` | 49.0% | 0.65 | 0.20 | 0.85 | 0.03 | 151 | 240.0 | ARCA:100%, BATS_INV:0%, IEX_SIM:0% |
| `venue_failover` | `naive_best_price` | 93.8% | 0.35 | 0.02 | 0.37 | 0.34 | 130 | 922.0 | ARCA:26%, BATS_INV:43%, IEX_SIM:30% |
| `venue_failover` | `fee_aware` | 98.9% | 0.41 | -0.03 | 0.38 | 0.14 | **3** | 898.1 | ARCA:13%, BATS_INV:60%, IEX_SIM:27% |
| `venue_failover` | `smart_cost_aware` | **99.2%** | 0.47 | -0.01 | 0.46 | **0.10** | **3** | **465.4** | ARCA:27%, BATS_INV:61%, IEX_SIM:12% |
| `mixed_stress` | `naive_primary` | 77.5% | 0.66 | 0.20 | 0.86 | 0.05 | 61 | 241.7 | ARCA:100%, BATS_INV:0%, IEX_SIM:0% |
| `mixed_stress` | `naive_best_price` | 96.2% | 0.36 | 0.05 | 0.41 | 0.30 | 53 | 901.5 | ARCA:43%, BATS_INV:37%, IEX_SIM:20% |
| `mixed_stress` | `fee_aware` | 97.6% | 0.39 | -0.01 | **0.38** | 0.20 | **3** | 884.9 | ARCA:23%, BATS_INV:57%, IEX_SIM:20% |
| `mixed_stress` | `smart_cost_aware` | **99.3%** | 0.43 | 0.03 | 0.46 | **0.10** | **3** | **443.1** | ARCA:44%, BATS_INV:50%, IEX_SIM:6% |

### Key Measured Tradeoff: Fee Minimization vs. Fill Certainty & Completion Latency
Comparing `fee_aware` and `smart_cost_aware` on `mixed_stress` highlights a real execution-engine tradeoff:
- `fee_aware` achieves the lowest realized net IS (**0.38 bps**, collecting `-0.01 bps` net rebate) by aggressively targeting cheaper/rebate venues (`BATS_INV` 57%, `IEX_SIM` 20%), at the expense of higher cancel/fill (**0.20**), lower fill ratio (**97.6%**), and slower median completion (**884.9 us**).
- `smart_cost_aware` shifts volume away from the slow `IEX_SIM` venue (`20% -> 6%`) toward fast `ARCA` (`23% -> 44%`), trading **+0.08 bps** of realized spread/fee cost for **2.0x lower cancel/fill (0.10)**, **2.0x faster parent completion (443.1 us)**, and **+1.7% higher fill ratio (99.3%)**.
- Both vastly outperform `naive_primary` (77.5% fill, 0.86 bps net IS, 61 failovers) and `naive_best_price` (96.2% fill, 0.30 cancel/fill, 53 failovers because it lacks venue circuit-breaker isolation).

## 2. Sensitivity Sweeps

Source: [sensitivity.json](../results/experiments/sensitivity.json)

### Quote Staleness Timeout (`stale_feed_trap`, `smart_cost_aware`)
| Timeout (ms) | Fill % | Net IS (bps) | Cxl/Fill | Med Lat (us) |
|---:|---:|---:|---:|---:|
| 1.0 | 97.0% | 0.51 | 0.09 | 453.8 |
| 2.0 | 99.4% | 0.51 | 0.08 | 462.6 |
| 5.0 | 99.4% | 0.51 | 0.08 | 467.5 |
| 100.0 | 99.4% | 0.51 | 0.08 | 467.5 |
| 5000.0 | 91.9% | 0.50 | 0.13 | 453.2 |

*Takeaway:* Too tight (`1.0 ms`, tighter than the 2 ms quote cadence) causes false-positive staleness exclusions (`97.0%` fill); too loose (`5000.0 ms`, longer than the freeze window) routes into frozen quotes and degrades fill ratio to `91.9%` while increasing cancel/fill by 62%.

### Fee Spread Multiplier (`fee_inversion`)
| Fee Scale | Naive Net IS (bps) | Smart Net IS (bps) | Savings (bps) | Naive Fee (bps) | Smart Fee (bps) |
|---:|---:|---:|---:|---:|---:|
| 0.5x | 0.41 | 0.39 | 0.02 | 0.06 | 0.04 |
| 1.0x | 0.47 | 0.39 | 0.08 | 0.12 | 0.03 |
| 1.5x | 0.53 | 0.37 | 0.16 | 0.19 | 0.00 |
| 2.0x | 0.59 | 0.38 | 0.21 | 0.25 | -0.05 |

### Latency Asymmetry Multiplier (`latency_race_depletion`)
| Latency Scale | Naive Fill% | Smart Fill% | Naive Cxl/Fill | Smart Cxl/Fill | Naive MedLat (us) | Smart MedLat (us) |
|---:|---:|---:|---:|---:|---:|---:|
| 0.5x | 99.9% | 100.0% | 0.26 | 0.03 | 2235.7 | 445.1 |
| 1.0x | 98.8% | 99.6% | 0.49 | 0.09 | 4514.1 | 880.4 |
| 2.0x | 93.7% | 98.9% | 0.69 | 0.18 | 8955.6 | 329.1 |
| 4.0x | 90.4% | 98.0% | 1.04 | 0.16 | 18487.9 | 650.0 |

### Displayed Liquidity Scale (`mixed_stress`, `smart_cost_aware`)
| Liquidity Scale | Fill % | Gross IS (bps) | Net IS (bps) | Cxl/Fill | Med Lat (us) |
|---:|---:|---:|---:|---:|---:|
| 0.4x | 99.1% | 0.75 | 0.78 | 0.12 | 864.1 |
| 0.7x | 98.5% | 0.51 | 0.53 | 0.11 | 467.7 |
| 1.0x | 99.3% | 0.43 | 0.46 | 0.10 | 443.1 |
| 1.5x | 98.7% | 0.37 | 0.40 | 0.11 | 439.0 |
