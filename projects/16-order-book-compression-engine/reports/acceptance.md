# Acceptance Report — Order Book Compression Engine

**Status:** Verified on stated environment (with optional external dependencies explicitly documented)  
**Environment:** Apple M5 (arm64, 10 logical CPUs), Darwin 25.5.0, AppleClang 21.0.0, CMake 4.5.1, Release (`-O3`, C++20)  
**Raw Environment Metadata:** [`reports/environment.json`](environment.json)

---

## 1. Project-Specific Acceptance Criteria

| Acceptance Criterion | Status | Evidence & Verification |
|---|---|---|
| **Byte-identical decoded logical events** | **PASS** | Verified across all 22 automated CTest cases (`reports/test_output.txt`), canonical JSONL fixtures (`diff -u` zero diff through CLI `encode -> decode`), and all 11 benchmark configurations (`lossless_verified: true`, matching CRC32C digests in `reports/benchmark_results.json`, e.g., `0x838AA9B9` for `l3_order_flow`, `0x91D73626` for `l2_top_of_book`, `0x7379AB21` for `volatile_bursty`). |
| **Reject corrupt frames** | **PASS** | Verified in `test_reference_codec::reference_crc_corruption_rejected`, `test_delta_codec::fuzzed_malformed_payloads_rejected_safely` (250 bit-flip mutations + 200 random garbage buffers), `test_integration_resilience::truncation_at_every_byte_offset_handled`, and `obce_cli demo` (`Corrupt-frame strict: rejected with CRC_MISMATCH`). |
| **Report bytes/event and p99 encode/decode times** | **PASS** | Measured across 3 synthetic workloads (50,000 events each) comparing reference binary vs delta+varint across checkpoint intervals `64`, `256`, `1024`, `4096`. Raw JSON in [`reports/benchmark_results.json`](benchmark_results.json) and CSV in [`reports/benchmark_summary.csv`](benchmark_summary.csv). |
| **All required test scenarios pass (or unavailable dependencies marked not run)** | **PASS** | 5/5 CTest suites (22/22 test cases) pass in 1.13s (`reports/test_output.txt`). Optional `libzstd` and Google Benchmark are **not installed** in this environment and are explicitly marked below. |
| **README documents setup, demo, validation, assumptions, and limitations** | **PASS** | Documented in [`README.md`](../README.md) with explicit distinction between observed measurements on synthetic workloads vs production feed performance. |
| **No real transactions, customer data, credentials, or proprietary/SAAT artifacts** | **PASS** | All fixtures and benchmarks use deterministic synthetic generators (`SplitMix64/Xoshiro256**`) and hand-crafted canonical JSONL fixtures (`fixtures/*.jsonl`). |

---

## 2. Test & Validation Matrix Coverage

| Test Area | Required Scenario | Test Suite / Case | Measured Result |
|---|---|---|---|
| **Domain correctness** | Roundtrip equality & fuzzed malformed payloads | `test_varint_crc` (5 cases), `test_reference_codec` (4 cases), `test_delta_codec` (4 cases) | **PASS** — Exact field equality across L2/L3 v1/v2; 250 bit-flips + 200 random buffers rejected safely; Castagnoli CRC32C check vector `"123456789" -> 0xE3069283` verified. |
| **Integration & resilience** | Extreme deltas, overflow & truncation | `test_integration_resilience::extreme_deltas_and_wrap_safe_overflow_roundtrip`, `truncation_at_every_byte_offset_handled`, `sequence_gap_policies_strict_allow_and_resync`, `schema_evolution_v1_to_v2_and_unsupported_version` | **PASS** — `INT64_MIN <-> INT64_MAX`, `UINT64_MAX`, `UINT32_MAX` roundtrip without UB; truncation at every non-boundary byte offset returns `TRUNCATED_INPUT`; v1 upgrades cleanly to v2; v99 rejected with `UNSUPPORTED_SCHEMA_VERSION`. |
| **Boundary & failure behavior** | Recovery from missing delta frame | `test_integration_resilience::recovery_from_missing_delta_frame` | **PASS** — Deleting an intermediate delta frame fails explicitly under `StrictReject` (`SEQUENCE_GAP`), and under `ResyncAtCheckpoint` resynchronizes at the next valid checkpoint (`resync_recoveries=1`), recovering all 145 unaffected events (`0..16` and `64..191`) identically. |
| **Reproducibility** | Versioned inputs/config & fixed seed | `test_operational::reproducibility_byte_identical_across_reruns`, `encoder_reuse_after_reset_is_idempotent` | **PASS** — Identical seeds produce byte-identical encoded streams and identical CRC32C digests (`0x1756E2FE` for demo). |
| **Operational review** | Missing config/files, unsupported inputs, rerun | `test_operational::invalid_configuration_rejected_explicitly`, `missing_files_and_unsupported_features_diagnostics` | **PASS** — Invalid configs return `INVALID_ARGUMENT` / `UNSUPPORTED_SCHEMA_VERSION`; missing files return `IO_ERROR`; unavailable zstd returns `UNSUPPORTED_FEATURE` (no silent success). |

---

## 3. Summary of Measured Benchmark Results (Synthetic Workloads, 50,000 Events)

All measurements below are **actual observations** on Apple M5 (`Release`, `AppleClang 21.0.0`, `std::chrono::steady_clock`, 2 warmup runs + 5 measured runs over 256-event chunks, 1,000 random-access seek queries):

| Workload (Synthetic) | Codec | Checkpoint Interval | Bytes / Event | Ratio vs Ref | Encode p99 (ns/ev) | Decode p99 (ns/ev) | Random Seek p99 (ns) | Mean Events Scanned |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `l3_order_flow` | `reference_binary_v2` | — | 59.38 | 1.00x | 137.76 | 143.55 | — | — |
| `l3_order_flow` | `delta_varint_v2` | 64 | 13.54 | 4.38x | 70.50 | 52.57 | 2,542.00 | 32.71 |
| `l3_order_flow` | `delta_varint_v2` | 256 | 12.33 | 4.81x | 52.42 | 40.04 | 9,755.41 | 127.01 |
| `l3_order_flow` | `delta_varint_v2` | 1024 | 12.03 | 4.94x | 51.76 | 39.42 | 34,583.42 | 518.63 |
| `l3_order_flow` | `delta_varint_v2` | 4096 | 11.95 | 4.97x | 64.40 | 49.91 | 137,584.67 | 2,068.63 |
| `l2_top_of_book` | `reference_binary_v2` | — | 59.38 | 1.00x | 125.31 | 129.10 | — | — |
| `l2_top_of_book` | `delta_varint_v2` | 256 | 12.40 | 4.79x | 64.36 | 49.07 | 8,625.00 | 129.04 |
| `l2_top_of_book` | `delta_varint_v2` | 1024 | 12.09 | 4.91x | 61.57 | 48.83 | 35,048.25 | 510.31 |
| `volatile_bursty` | `reference_binary_v2` | — | 59.38 | 1.00x | 128.18 | 136.58 | — | — |
| `volatile_bursty` | `delta_varint_v2` | 256 | 23.18 | 2.56x | 103.06 | 80.89 | 15,877.50 | 126.20 |
| `volatile_bursty` | `delta_varint_v2` | 1024 | 22.91 | 2.59x | 103.39 | 83.76 | 61,625.41 | 518.67 |

---

## 4. Explicitly Untested / Unavailable Optional Capabilities

- **Optional `libzstd` secondary block compression (`ZstdAdapter`):** `libzstd` headers/libraries were not installed in this environment (`OBCE_HAVE_ZSTD=0`). The fallback error path (`ErrorCode::UnsupportedFeature`) **was** tested and verified; live `libzstd` compression ratios and latency are **not run** here.
- **Google Benchmark harness:** Not installed offline; replaced by the built-in zero-dependency `obce_bench` percentile harness (`benchmarks/bench_main.cpp`).
- **Live exchange / production market data feeds:** Explicitly out of scope; all benchmarks were run on deterministic synthetic workloads.
