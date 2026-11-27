# Data Contracts — Kernel Bypass Networking Lab

**Contract Version:** `1`  
**Applies to:** `kbnl_core`, `kbnl_sender`, `kbnl_receiver`, `kbnl_bench`, and all `fixtures/` and `reports/` artifacts.

---

## 1. Synthetic Sequenced UDP Wire Protocol (v1)

All multi-byte header fields are encoded in **network byte order (big-endian)**. Minimum packet size is **64 bytes** (`kMinPacketBytes`); maximum UDP payload size is **1,472 bytes** (`kMaxPacketBytes`, matching 1,500-byte Ethernet MTU minus 20-byte IPv4 and 8-byte UDP headers).

| Byte Offset | Size (bytes) | Field | Type | Semantics & Units |
|---|---|---|---|---|
| `0..3` | 4 | `magic` | `uint32_t` | Fixed magic `0x4B424E4C` (`"KBNL"`) |
| `4..5` | 2 | `version` | `uint16_t` | Protocol version, must equal `1` |
| `6..7` | 2 | `flags` | `uint16_t` | Bitmask: `0x0001`=`WARMUP`, `0x0002`=`EOS`, `0x0004`=`BURST` |
| `8..11` | 4 | `feed_id` | `uint32_t` | Logical feed identifier |
| `12..15` | 4 | `payload_len` | `uint32_t` | Payload length in bytes (`28 <= payload_len <= 1436`) |
| `16..23` | 8 | `seq_num` | `uint64_t` | 1-based monotonic sequence (`>= 1` for data; `0` allowed only with `EOS`) |
| `24..31` | 8 | `send_ts_ns` | `uint64_t` | Sender monotonic timestamp in **nanoseconds** (`CLOCK_MONOTONIC_RAW`) |
| `32..35` | 4 | `crc32` | `uint32_t` | IEEE 802.3 CRC32 over header bytes `[0..31]` concatenated with payload `[36..36+payload_len-1]` |
| `36..N-1` | `payload_len` | `payload` | `uint8_t[]` | Deterministic splitmix64 byte stream seeded by `(seed, seq_num)` |

### Wire Validation Error Codes (`DecodeError`)
- `OK` — Valid header, length, sequence, and CRC32.
- `TRUNCATED` — Datagram shorter than 64 bytes.
- `BAD_MAGIC` — First 4 bytes != `0x4B424E4C`.
- `UNSUPPORTED_VERSION` — `version != 1`.
- `LENGTH_MISMATCH` — `36 + payload_len != datagram_size` or exceeds 1,472 bytes.
- `INVALID_SEQUENCE` — `seq_num == 0` on a non-`EOS` datagram.
- `BAD_CHECKSUM` — Computed CRC32 does not match header `crc32`.

---

## 2. Benchmark Configuration Input Contract (JSON v1)

Configuration files accept top-level `"workload"` and `"topology"` objects.

### `workload` fields
| Field | Type | Units / Allowed Values | Validation Rule |
|---|---|---|---|
| `schema_version` | integer | `1` | Must equal `1` |
| `scenario_name` | string | identifier | Non-empty |
| `feed_id` | integer | `uint32` | `>= 0` |
| `seed` | integer | `uint64` | Fixed seed for deterministic payloads |
| `packet_bytes` | integer | bytes | `[64, 1472]` |
| `warmup_packets` | integer | packets | `>= 0` (excluded from latency and measured throughput) |
| `measured_packets` | integer | packets | `>= 1` |
| `target_pps` | integer | packets/sec | `>= 1` |
| `pacing_mode` | string | `constant_rate` \| `burst` | Must match enum |
| `burst_size` | integer | packets/burst | `>= 2` when `pacing_mode == "burst"` |
| `burst_interval_us` | integer | microseconds | `>= 1` when `pacing_mode == "burst"` |
| `drain_timeout_ms` | integer | milliseconds | `[1, 60000]` |
| `batch_size` | integer | frames/poll | `[1, 1024]` |
| `backend` | string | `posix_udp_blocking` \| `posix_udp_busy_poll` \| `posix_udp_recvmmsg` \| `af_xdp` \| `dpdk` | Must match enum |

### `topology` fields
| Field | Type | Units / Allowed Values | Validation Rule |
|---|---|---|---|
| `bind_ip` / `target_ip` | string | IPv4 dotted quad | Validated via `inet_pton(AF_INET)` |
| `port` | integer | UDP port | `0` (ephemeral loopback) or `1..65535` |
| `interface_name` | string | OS interface name | Non-empty (`lo0`, `eth0`, etc.) |
| `rx_queue_id` | integer | NIC queue index | `>= 0` |
| `sender_cpu` / `receiver_cpu` | integer | logical CPU id | `>= -1` (`-1` = unpinned) |
| `pin_threads` | boolean | `true` \| `false` | If `true`, at least one CPU id must be `>= 0` |
| `strict_pinning` | boolean | `true` \| `false` | If `true` and OS cannot strictly pin, run fails explicitly |
| `socket_rcvbuf_bytes` / `socket_sndbuf_bytes` | integer | bytes | `>= 4096` |
| `ring_capacity` | integer | slots | Power of two `>= 2` |

### Structured Validation & Runtime Error Codes
- Config errors: `JSON_PARSE_ERROR`, `TYPE_ERROR`, `RANGE_ERROR`, `UNSUPPORTED_SCHEMA`, `UNSUPPORTED_VALUE`, `UNSUPPORTED_BACKEND`, `EMPTY_VALUE`, `INVALID_IP`, `INCONSISTENT_CONFIG`, `INVALID_CAPACITY`, `FILE_NOT_FOUND`.
- Transport/runtime `StatusCode`: `OK`, `INVALID_CONFIG`, `UNSUPPORTED_PLATFORM`, `FEATURE_DISABLED`, `MISSING_CAPABILITY`, `DEVICE_UNAVAILABLE`, `BIND_FAILED`, `IO_ERROR`, `TIMEOUT`.

---

## 3. Binary Fixture Container Format (`KBNLFIX1`)

Used by `fixtures/deterministic_feed_v1.bin` and `kbnl_bench --replay-fixture`:
- File header (16 bytes): 8-byte ASCII magic `"KBNLFIX1"`, `uint32_t record_count` (little-endian), `uint32_t expected_total_sent` (little-endian).
- Repeated `record_count` times:
  - `uint64_t recv_ts_ns` (little-endian)
  - `uint32_t frame_len` (little-endian, `<= 1472`)
  - `uint8_t frame_bytes[frame_len]`

---

## 4. Benchmark Output Contract (JSON v1)

Every report written to `reports/*.json` contains:
- **Identity & provenance:** `schema_version`, `run_id` (`<scenario>_<backend>_<8-hex-config-hash>`), `config_hash` (64-bit FNV-1a), `timestamp_utc`, `scenario_name`, `backend`.
- **Honesty & execution labeling:**
  - `execution_status`: `"MEASURED"` (live socket/NIC run), `"NOT_RUN"` (platform/feature/privilege unavailable), `"FIXTURE_VERIFICATION"` (deterministic binary replay), or `"FAILED"`.
  - `is_simulated`: always `false` (no simulated hardware measurements are ever produced).
- **Environment & clock metadata:** `capability`, `clock` (`name`, `api`, `nominal_resolution_ns`, `measured_call_overhead_ns`, `same_host_domain_required`), `pinning.sender`, `pinning.receiver`.
- **Throughput metrics:** `expected_warmup_sent`, `expected_measured_sent`, `actual_packets_sent`, `send_errors`, `measured_duration_ns`, `achieved_send_pps`, `achieved_recv_pps`, `achieved_recv_mbps`.
- **Sequence accounting:** `total_valid_data_packets`, `unique_received`, `warmup_received`, `measured_received`, `burst_packets`, `gaps_events`, `missing_within_highest`, `inferred_total_lost`, `loss_ratio`, `out_of_order`, `duplicates`, `too_old_out_of_window`, `checksum_errors`, `decode_errors`, `ring_drops`, `eos_packets`, `first_seq`, `highest_seq`, `last_seq`, `accounting_invariant_holds`, `sequence_accounting_verified`.
- **Latency (`latency_ns`):** `count`, `warmup_ignored`, `negative_clamped`, `exact_quantiles`, `min`, `mean`, `stddev`, `p50`, `p95`, `p99`, `p99_9`, `max` (all in nanoseconds).

---

## 5. Fixture Catalog

| Fixture Path | Category | Purpose |
|---|---|---|
| `fixtures/valid_baseline_config.json` | Valid | Canonical 64-byte constant-rate loopback configuration |
| `fixtures/valid_burst_config.json` | Valid | Canonical 128-byte burst-load configuration |
| `fixtures/invalid_configs/zero_rate.json` | Invalid | Boundary rejection: `target_pps = 0` |
| `fixtures/invalid_configs/bad_payload_size.json` | Invalid | Boundary rejection: `packet_bytes = 32 < 64` |
| `fixtures/invalid_configs/unsupported_backend.json` | Invalid | Rejection of unknown backend identifier |
| `fixtures/invalid_configs/invalid_cpu_pin.json` | Invalid | Inconsistent `pin_threads=true` + non-power-of-two `ring_capacity=1000` |
| `fixtures/deterministic_feed_v1.bin` + `.json` | Deterministic reference | 12-packet synthetic stream with warmup, loss, out-of-order, duplicate, CRC error, EOS |
| `fixtures/expected_reference_result.json` | Independent oracle | Hand-computed exact counters and quantiles verified by CTest & `verify_acceptance.py` |
