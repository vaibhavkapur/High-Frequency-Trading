#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kbnl {

enum class TransportBackend {
    kPosixUdpBlocking = 0,
    kPosixUdpBusyPoll,
    kPosixUdpRecvmmsg,
    kAfXdp,
    kDpdk
};

enum class PacingMode {
    kConstantRate = 0,
    kBurst
};

std::string_view backend_name(TransportBackend backend) noexcept;
bool parse_backend_name(std::string_view name, TransportBackend& out) noexcept;

std::string_view pacing_mode_name(PacingMode mode) noexcept;
bool parse_pacing_mode(std::string_view name, PacingMode& out) noexcept;

struct TopologyConfig {
    std::string bind_ip = "127.0.0.1";
    std::string target_ip = "127.0.0.1";
    uint16_t port = 0; // 0 = ephemeral OS-assigned port (loopback benchmark)
    std::string interface_name = "lo0";
    uint32_t rx_queue_id = 0;
    int sender_cpu = -1;   // -1 = unpinned
    int receiver_cpu = -1; // -1 = unpinned
    bool pin_threads = false;
    bool strict_pinning = false;
    uint32_t socket_rcvbuf_bytes = 4 * 1024 * 1024;
    uint32_t socket_sndbuf_bytes = 4 * 1024 * 1024;
    size_t ring_capacity = 65536;
};

struct WorkloadConfig {
    uint32_t schema_version = 1;
    std::string scenario_name = "baseline_loopback";
    uint32_t feed_id = 1;
    uint64_t seed = 20261127ULL;
    size_t packet_bytes = 64;
    uint64_t warmup_packets = 1000;
    uint64_t measured_packets = 10000;
    uint64_t target_pps = 10000;
    PacingMode pacing_mode = PacingMode::kConstantRate;
    uint64_t burst_size = 1;
    uint64_t burst_interval_us = 1000;
    uint32_t drain_timeout_ms = 250;
    size_t batch_size = 32;
    TransportBackend backend = TransportBackend::kPosixUdpBlocking;
};

struct BenchmarkConfig {
    WorkloadConfig workload{};
    TopologyConfig topology{};
    std::string output_path{};
    std::string fixture_path{};
};

struct ValidationError {
    std::string field;
    std::string code;
    std::string message;
};

std::vector<ValidationError> validate_config(const BenchmarkConfig& config);

// Loads config from a JSON file (flat or nested with "workload" / "topology").
// Returns true on parse success; semantic errors are still checked via validate_config.
bool load_config_from_json_string(
    const std::string& json_text,
    BenchmarkConfig& out_config,
    std::vector<ValidationError>& out_errors);

bool load_config_from_json_file(
    const std::string& path,
    BenchmarkConfig& out_config,
    std::vector<ValidationError>& out_errors);

// Deterministic 64-bit FNV-1a hex fingerprint of canonical config parameters.
std::string compute_config_hash(const BenchmarkConfig& config);

} // namespace kbnl
