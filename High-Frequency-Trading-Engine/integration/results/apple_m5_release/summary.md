# Benchmark Summary

- **Git SHA:** `8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89`
- **Host:** `Apple M5` (`arm64`, 10 logical cores), `macOS 26.5.1`
- **Compiler / Build:** `AppleClang 21.0.0.21000101` (`Release`, flags: `-O3 -DNDEBUG`)
- **Clock overhead:** `19.17 ns`

## Stage & Comparative Benchmarks

| Suite | Benchmark | Variant | p50 (ns/op) | p99 (ns/op) | Mops/s | RepCV% |
|---|---|---|---:|---:|---:|---:|
| stage | `01_itch_decode` | `project02_adapter` | 135.75 | 191.21 | 7.37 | 4.32% |
| stage | `02_book_update` | `canonical_l3_book` | 39.92 | 54.56 | 25.05 | 11.97% |
| stage | `03_strategy_compute` | `mmsim_inventory_mm` | 14.50 | 18.04 | 68.97 | 0.54% |
| stage | `04_pretrade_risk` | `risk_gate_check` | 107.17 | 130.21 | 9.33 | 4.71% |
| stage | `05_smart_routing` | `sor_cost_aware_3v` | 639.04 | 963.37 | 1.56 | 20.15% |
| stage | `06_wire_serialization` | `ouch_roundtrip` | 22.42 | 22.71 | 44.61 | 0.17% |
| stage | `07_matching_steady` | `pool_book_10k` | 16.24 | 27.99 | 61.58 | 11.37% |
| stage | `07_matching_steady` | `map_book_10k` | 34.12 | 49.68 | 29.31 | 11.84% |
| stage | `08_oms_and_ledger` | `fixed_point_ledger` | 96.79 | 111.56 | 10.33 | 3.10% |
| e2e | `full_pipeline_deterministic` | `direct_pool_book` | 2750.78 | 3181.81 | 0.36 | 4.88% |
| e2e | `full_pipeline_deterministic` | `direct_map_book` | 234.81 | 295.38 | 4.26 | 3.89% |
| e2e | `feed_transport_compare` | `direct_in_process` | 15.75 | 17.69 | 63.49 | 0.39% |
| e2e | `feed_transport_compare` | `spsc_two_thread` | 54.46 | 84.02 | 18.36 | 7.48% |
| e2e | `multicast_mdf1_compare` | `clean_no_loss` | 253.12 | 278.13 | 3.95 | 1.87% |
| e2e | `multicast_mdf1_compare` | `loss_15pct_recovered` | 310.75 | 322.46 | 3.22 | 0.51% |

## End-to-End Distribution Breakdown (Wall-Clock Monotonic ns)

| Metric | Samples | p50 (ns) | p95 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---:|---:|---:|---:|---:|---:|
| `tick_to_decision` | 500 | 333.0 | 584.0 | 1125.0 | 2938.8 | 3583.0 |
| `order_submit_to_ack` | 500 | 125.0 | 208.0 | 791.4 | 3023.4 | 4458.0 |
| `order_submit_to_fill` | 500 | 166.5 | 250.0 | 875.8 | 4752.3 | 6166.0 |

- **Pool vs Map Checksum Match:** `True`
- **Direct vs SPSC Checksum Match:** `True` (drops: `0`)
