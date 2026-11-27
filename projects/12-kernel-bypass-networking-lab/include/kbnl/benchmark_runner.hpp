#pragma once

#include "kbnl/clock.hpp"
#include "kbnl/config.hpp"
#include "kbnl/cpu_affinity.hpp"
#include "kbnl/latency_stats.hpp"
#include "kbnl/sequence_tracker.hpp"
#include "kbnl/transport.hpp"

#include <cstdint>
#include <string>

namespace kbnl {

struct BenchmarkResult {
    uint32_t schema_version = 1;
    std::string run_id;
    std::string config_hash;
    std::string timestamp_utc;
    std::string scenario_name;
    std::string backend;
    std::string execution_status; // "MEASURED", "NOT_RUN", "FAILED"
    bool is_simulated = false;
    Status status = Status::Ok();
    CapabilityReport capability{};
    ClockMetadata clock{};
    PinResult sender_pin{};
    PinResult receiver_pin{};

    uint64_t expected_warmup_sent = 0;
    uint64_t expected_measured_sent = 0;
    uint64_t actual_packets_sent = 0;
    uint64_t send_errors = 0;
    uint64_t measured_duration_ns = 0;
    double achieved_send_pps = 0.0;
    double achieved_recv_pps = 0.0;
    double achieved_recv_mbps = 0.0;

    SequenceCounters sequence{};
    uint64_t inferred_total_lost = 0;
    double loss_ratio = 0.0;
    bool sequence_accounting_verified = false;

    LatencySummary latency{};

    BenchmarkConfig config{};
};

// Runs a complete sender -> receiver benchmark on the configured backend.
// If the backend is unavailable (e.g. AF_XDP / DPDK on macOS or unprivileged host),
// returns execution_status = "NOT_RUN" with explicit StatusCode and capability metadata
// rather than simulating or faking hardware results.
BenchmarkResult run_benchmark(const BenchmarkConfig& config);

// Replays a deterministic binary packet fixture through the pure domain pipeline
// (decode -> SPSC ring -> SequenceTracker -> LatencyStats) without network sockets,
// marking execution_status = "FIXTURE_VERIFICATION" and is_simulated = false (deterministic oracle check).
BenchmarkResult run_fixture_replay(const std::string& fixture_path, size_t ring_capacity = 65536);

} // namespace kbnl
