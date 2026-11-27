# Data Contracts and Schemas (`ptsp.v1`)

All versioned inputs, configurations, and outputs carry `schema_version = "ptsp.v1"`. Missing or mismatched schema versions fail closed with `SCHEMA_VERSION_MISMATCH`.

## 1. Units and Timestamp Semantics

| Quantity | Type | Unit | Semantics |
|---|---|---|---|
| Timestamps (`value_ns`, `t1_ns`..`t4_ns`) | `int64` | nanoseconds (`ns`) | Strictly positive (`> 0`) integer nanoseconds since the domain's epoch |
| Uncertainty (`uncertainty_ns`, `total_uncertainty_ns`) | `int64` | nanoseconds (`ns`) | Non-negative (`>= 0`) half-width (`+/- ns`) |
| Clock offset (`offset_ns`) | `int64` | nanoseconds (`ns`) | `local_clock - reference_clock` at `anchor_ref_ns` |
| Clock drift (`drift_ppb`) | `float64` | parts-per-billion (`ppb` = `ns/s`) | Rate of change of `offset_ns` per reference second |
| Round-trip delay (`raw_rtt_ns`) | `int64` | nanoseconds (`ns`) | `(t4 - t1) - (t3 - t2)`, must be `>= 0` |

### Event-Time vs Receive-Time Contract

- **`EVENT_TIME`**: Authoritative timestamp assigned when the market event occurred (e.g., matching engine or grandmaster-locked feed source). Used as the canonical ordering key.
- **`RECEIVE_TIME`**: Local host/NIC timestamp recorded when the packet arrived. Used to measure transit latency and verify causal consistency (`aligned_receive_time >= event_time` within uncertainty + causal slack), never silently substituted for `EVENT_TIME`.
- **Provenance fields** (mandatory on every timestamp):
  - `domain`: `TAI`, `UTC`, `MONOTONIC_RAW`, `MONOTONIC`, `EXCHANGE_VENUE` (`UNKNOWN` is rejected).
  - `role`: `EVENT_TIME`, `RECEIVE_TIME`, `TRANSMIT_TIME`.
  - `capture_method`: `HARDWARE_NIC_PTP`, `KERNEL_SOFTWARE`, `USERSPACE_CLOCK`, `SYNTHETIC_FIXTURE`, `RECORDED_REPLAY` (`UNKNOWN` is rejected).
  - `error_source`: `MEASURED`, `SYNTHETIC_FIXTURE`, `ESTIMATED_MODEL`.
  - `clock_id`: Non-empty identifier of the physical or synthetic clock instance.
  - `epoch_id`: `uint32` incremented on clock step or restart.

## 2. Input Schemas

### 2.1 Scenario Configuration (`*.cfg`, key=value)

Required header/key: `schema_version=ptsp.v1`. Unknown keys fail closed with `INVALID_CONFIG`.

Supported keys:
- `scenario_name`, `seed`, `sync_fixture_path`, `packet_fixture_path`
- Estimator: `window_size` (`[2, 128]`), `outlier_mad_multiplier` (`> 0`), `rtt_spike_factor` (`>= 1.0`), `step_threshold_ns` (`> 0`), `step_confirm_count` (`[1, 8]`)
- Orderer: `expected_event_domain`, `expected_receive_domain`, `max_buffered_events` (`[1, 1000000]`), `reorder_window_ns` (`>= 0`), `max_skew_ns` (`> 0`), `max_causal_slack_ns` (`>= 0`), `fail_closed_on_ambiguity` (`true`/`false`)
- Acceptance envelopes: `max_allowed_offset_error_ns`, `max_allowed_drift_error_ppb`

### 2.2 Synchronization Exchange Fixture (`*_sync.csv`)

Must include comment header `# schema_version=ptsp.v1` and 15 columns:
```text
seq,epoch_id,reference_domain,local_domain,reference_clock_id,local_clock_id,capture_method,error_source,t1_ns,t2_ns,t3_ns,t4_ns,true_offset_ns,true_drift_ppb,true_asymmetry_ns
```
The final three ground-truth columns are optional (empty in live/replay captures) and are **never** read by estimators — only by the offline evaluation harness.

### 2.3 Timestamped Packet Fixture (`*_packets.csv`)

Must include comment header `# schema_version=ptsp.v1` and 18 columns:
```text
packet_id,sequence_id,source_id,symbol,event_ns,event_unc_ns,event_domain,event_role,event_capture,event_error_source,event_clock_id,rx_ns,rx_unc_ns,rx_domain,rx_role,rx_capture,rx_error_source,rx_clock_id
```

## 3. Output Contracts

- **Scenario Evaluation JSON (`reports/scenario_<name>.json`)**: Includes `schema_version`, `scenario_name`, deterministic 64-bit hex `run_fingerprint`, `error_source`, `baseline_estimator` metrics, `robust_estimator` metrics (including `steady_state_max_error_ns` and `envelope_coverage_ratio`), and `orderer` state + counters.
- **Audit Log JSONL (`reports/audit_<name>.jsonl`)**: One JSON object per diagnostic decision (`packet_id`, `sequence_id`, `decision`, `error_code`, `reason`).
- **Capability Probe JSON (`reports/host_probe.json`)**: Separate sections for `software_adapter` (`MEASURED`) and `linux_ptp_hw_adapter` (`MEASURED`, reporting `HARDWARE_TIMESTAMP_UNAVAILABLE` if unsupported).

## 4. Error Codes

| Code | Meaning | Behavior |
|---|---|---|
| `MISSING_CONFIG` / `INVALID_CONFIG` | Missing file, bad value, or unknown key | Fail closed at startup (non-zero exit) |
| `SCHEMA_VERSION_MISMATCH` | Missing or unsupported `schema_version` | Fail closed before parsing rows |
| `INVALID_TIMESTAMP` / `INVALID_SYNC_SAMPLE` | Non-positive timestamp, negative uncertainty, swapped role, or negative RTT | Reject record; increment validation counter |
| `CLOCK_DOMAIN_MISMATCH` / `UNSUPPORTED_CLOCK_DOMAIN` | Mixed domains (e.g., TAI + UTC) or unsupported clock domain | Reject explicitly; never silently convert |
| `HARDWARE_TIMESTAMP_UNAVAILABLE` | No Linux PHC / NIC HW timestamping capability | Explicit error; never fabricate HW timestamps |
| `SKEW_VIOLATION` / `CAUSAL_VIOLATION` / `AMBIGUOUS_ORDERING` | Envelope exceeded, receive precedes event, or overlapping cross-source intervals | Transition orderer to `HALTED_FAIL_CLOSED` |
| `BUFFER_CAPACITY_EXCEEDED` | Reorder buffer reached `max_buffered_events` | Explicit backpressure error; no silent drops |
| `FAIL_CLOSED_HALTED` | Packet/flush attempted while orderer is halted | Reject until explicit operator recovery |

## 5. Valid, Invalid, and Boundary Fixtures

| Fixture | Category | Purpose |
|---|---|---|
| `fixtures/reference_drift_*` | Valid | Clean 150 ppb drift, symmetric jitter, ordered stream |
| `fixtures/clock_step_and_outliers_*` | Valid (fault-injected) | +25,000 ns clock step + 4 single-packet +18,000 ns spikes |
| `fixtures/packet_asymmetry_*` | Valid (fault-injected) | Periodic +12,000 ns forward asymmetry bursts |
| `fixtures/clock_restart_*` | Valid (fault-injected) | Epoch 0→1 restart with -500 ms local clock reset |
| `fixtures/out_of_order_stream_*` | Valid (integration) | Out-of-order arrivals within 2,000 ns reorder window |
| `fixtures/skew_violation_fail_closed_*` | Valid (safety fault) | -25,000 ns causal receive-before-event violation |
| `fixtures/boundary_zero_uncertainty_packets.csv` | Boundary | Zero uncertainty (`0 ns`) and zero transit delay (`0 ns`) |
| `fixtures/invalid_negative_rtt_sync.csv` | Invalid | `t4 < t1 + (t3 - t2)` implying negative RTT |
| `fixtures/invalid_mixed_domain_packets.csv` | Invalid | Mixes `TAI` and `UTC` event timestamps in one stream |
| `fixtures/invalid_schema.cfg`, `invalid_unknown_key.cfg` | Invalid | Bad schema version and unrecognized config key |
