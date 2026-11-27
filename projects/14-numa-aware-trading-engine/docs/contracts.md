# Data Contracts & Schema Specification

**Contract Version:** `1.0.0` (`numa_engine::kSchemaVersion`)

## 1. Canonical Input Event Schema (`MarketEvent`)

### In-Memory Layout (Hot Path)
- Exact size: **64 bytes** (`alignas(64)`, trivially copyable, standard layout).
- Price unit: **integer ticks** (`int64_t price_ticks`, where `1 tick = 0.0001 USD`). Integer representation eliminates floating-point rounding drift across architectures.
- Timestamp unit: **nanoseconds** (`uint64_t send_ts_ns`) sourced from `CLOCK_MONOTONIC_RAW` (fallback: `std::chrono::steady_clock`).

### CSV Fixture Schema
Header (mandatory, exact order):
```csv
seq_id,symbol_id,event_type,side,price_ticks,quantity,order_id,flags
```

| Field | Type | Units / Domain | Validation Rules |
|---|---|---|---|
| `seq_id` | `uint64` | Monotonic sequence | Must be `> 0` and strictly greater than previous `seq_id` (`NON_MONOTONIC_SEQUENCE`). |
| `symbol_id` | `uint16` | Symbol index `[0, 63]` | Must be `< 64` (`INVALID_SYMBOL`). |
| `event_type` | enum / `uint8` | `ADD` (1), `MODIFY` (2), `CANCEL` (3), `TRADE` (4) | Reject unknown values (`INVALID_EVENT_TYPE`). |
| `side` | enum / `uint8` | `BID`/`BUY` (1), `ASK`/`SELL` (2) | Reject unknown values (`INVALID_SIDE`). |
| `price_ticks` | `int64` | `0.0001 USD` / tick | `> 0` for `ADD`/`MODIFY`/`TRADE`; `>= 0` for `CANCEL` (`INVALID_PRICE`). |
| `quantity` | `uint32` | Synthetic shares/contracts | `> 0` for `ADD`/`MODIFY`/`TRADE`; `>= 0` for `CANCEL` (`0` = cancel full top-of-book quantity) (`INVALID_QUANTITY`). |
| `order_id` | `uint64` | Synthetic identifier | Any unsigned 64-bit integer. |
| `flags` | `uint32` | Bitmask | Bit 0 (`1`): warm-up event (updates domain state, excluded from latency/throughput metrics). |

## 2. Pipeline Configuration Schema (`PipelineConfig` JSON)

Required fields (missing any returns `INVALID_CONFIG` or `INVALID_QUEUE_CAPACITY` — no silent defaults):
- `scenario_name` (non-empty string)
- `pipeline_kind`: `"baseline_mutex"` | `"lock_free_spsc"`
- `memory_policy`: `"default"` | `"first_touch_local"` | `"local"` | `"remote"` | `"interleaved"`
- `queue_capacity`: integer; must be power-of-two in `[8, 4194304]` for `lock_free_spsc` (`INVALID_QUEUE_CAPACITY`)

Optional fields:
- `producer_cpu`, `consumer_cpu`: integer CPU ID (`-1` = unpinned; out-of-range fails with `INVALID_CPU_ID`)
- `preferred_memory_node`: integer NUMA node ID (`0` default; invalid node fails with `INVALID_NUMA_NODE`)
- `consumer_first_touch`: boolean (`true` default)
- `allow_single_node_fallback`: boolean (`true` default in single-run config; `false` default in strict comparison mode)
- `require_hard_pinning`: boolean (`false` default; if `true` on macOS, fails with `UNAVAILABLE_PLATFORM_API`)
- `consumer_work_iters`: uint32 synthetic per-event consumer spin iterations for backpressure testing
- `seed`: uint64 deterministic PRNG seed
- `warmup_events`: uint64 warm-up event count

## 3. Output Contracts

### Single Run (`PipelineRunResult` JSON)
Includes `schema_version`, deterministic `run_id` (`run-<fnv1a64>`), `scenario_name`, `pipeline_kind`, `requested_policy`, `effective_policy`, `status` (`EXECUTED` | `FALLBACK_SINGLE_NODE` | `UNAVAILABLE_SINGLE_NODE` | `UNAVAILABLE_PLATFORM` | `FAILED_VALIDATION`), `status_note`, `clock_source`, `producer_affinity`, `consumer_affinity`, `allocation`, event counts (`events_sent`, `events_received`, `warmup_events`, `measured_events`, `lost_events`), `elapsed_ms`, `throughput_eps`, `queue_latency` & `e2e_latency` (`min/p50/p90/p99/p99.9/p99.99/max/mean/stddev` in ns), `backpressure`, `perf_counters`, and `engine_summary` (including hex `final_state_digest`).

### Comparison Suite (`ComparisonReport` JSON & CSV)
Wraps `topology`, `build_type`, `compiler`, `clock_source`, `baseline_digest`, `functional_equivalence_passed`, `zero_lost_events`, and per-scenario aggregates (`mean/min/max_throughput_eps`, `median_p50/p99/p99_9_ns`, `total_lost_events`, `all_digests_match_baseline`, and raw per-repetition `runs[]`).

## 4. Error Codes

| Code | Name | Meaning |
|---:|---|---|
| 0 | `OK` | Success |
| 100 | `INVALID_CONFIG` | Missing or invalid configuration field |
| 101 | `INVALID_QUEUE_CAPACITY` | Queue capacity out of bounds or non-power-of-two for SPSC |
| 102 | `INVALID_CPU_ID` | CPU ID does not exist in host/target topology |
| 103 | `INVALID_NUMA_NODE` | Requested NUMA node does not exist |
| 200–206 | `INVALID_EVENT_*` / `NON_MONOTONIC_SEQUENCE` | Event schema violation (reports field name and CSV line number) |
| 300–301 | `FILE_NOT_FOUND` / `PARSE_ERROR` | Missing file or malformed CSV/JSON syntax |
| 400 | `UNAVAILABLE_SINGLE_NODE` | Multi-NUMA policy (`remote` / `interleaved`) requested on 1-node host in strict mode |
| 401 | `UNAVAILABLE_PLATFORM_API` | Hard CPU pinning or Linux NUMA syscall unavailable on host OS |
| 500–501 | `ALLOCATION_FAILED` / `AFFINITY_FAILED` | OS allocation or affinity syscall failure |
| 600 | `EVENT_LOSS_DETECTED` | Post-run count mismatch (`sent != received`) |

## 5. Fixture Catalog (`fixtures/`)

| Fixture | Category | Purpose |
|---|---|---|
| `valid_small_events.csv` | Valid | 12 deterministic synthetic events across 3 symbols exercising all 4 event types + 2 empty-side cancel no-ops |
| `expected_small_output.json` | Expected result | Independently hand-traced expected totals and per-symbol book states for `valid_small_events.csv` |
| `boundary_events.csv` | Boundary | Max `symbol_id=63`, `price_ticks=1`, large qty, full-book cancel (`qty=0`), warm-up flag bit |
| `invalid_negative_price.csv` | Invalid | Triggers `INVALID_PRICE` on line 4 (`price_ticks=-500`) |
| `invalid_non_monotonic_seq.csv` | Invalid | Triggers `NON_MONOTONIC_SEQUENCE` on line 5 (duplicate `seq_id=2`) |
| `invalid_unknown_type.csv` | Invalid | Triggers `INVALID_EVENT_TYPE` on line 3 (`SNAPSHOT`) |
| `config_valid.json` | Valid config | Valid `first_touch_local` JSON configuration |
| `config_invalid_missing_fields.json` | Invalid config | Triggers `INVALID_CONFIG` for missing required keys |
| `config_unsupported_remote_strict.json` | Boundary / Failure | Triggers `UNAVAILABLE_SINGLE_NODE` on 1-node hosts when `allow_single_node_fallback=false` |
