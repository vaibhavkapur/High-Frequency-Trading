#include "test_framework.hpp"

#include "kbnl/benchmark_runner.hpp"
#include "kbnl/config.hpp"

#include <string>

namespace {

kbnl::BenchmarkConfig make_loopback_cfg(kbnl::TransportBackend backend, kbnl::PacingMode mode) {
    kbnl::BenchmarkConfig cfg{};
    cfg.workload.scenario_name = "integration_loopback";
    cfg.workload.feed_id = 1;
    cfg.workload.seed = 20261127ULL;
    cfg.workload.packet_bytes = 64;
    cfg.workload.warmup_packets = 100;
    cfg.workload.measured_packets = 1000;
    cfg.workload.target_pps = 20000;
    cfg.workload.pacing_mode = mode;
    cfg.workload.burst_size = (mode == kbnl::PacingMode::kBurst) ? 16 : 1;
    cfg.workload.burst_interval_us = 1000;
    cfg.workload.drain_timeout_ms = 250;
    cfg.workload.batch_size = 32;
    cfg.workload.backend = backend;

    cfg.topology.bind_ip = "127.0.0.1";
    cfg.topology.target_ip = "127.0.0.1";
    cfg.topology.port = 0; // Ephemeral port to avoid collisions
    cfg.topology.pin_threads = true;
    cfg.topology.strict_pinning = false;
    cfg.topology.sender_cpu = 0;
    cfg.topology.receiver_cpu = 1;
    cfg.topology.ring_capacity = 65536;
    return cfg;
}

KBNL_TEST(compare_identical_payload_and_cpu_policy_across_supported_paths) {
    const auto cfg_blocking = make_loopback_cfg(
        kbnl::TransportBackend::kPosixUdpBlocking, kbnl::PacingMode::kConstantRate);
    const auto cfg_busy = make_loopback_cfg(
        kbnl::TransportBackend::kPosixUdpBusyPoll, kbnl::PacingMode::kConstantRate);

    const auto res_blocking = kbnl::run_benchmark(cfg_blocking);
    const auto res_busy = kbnl::run_benchmark(cfg_busy);

    KBNL_EXPECT_EQ(res_blocking.execution_status, std::string("MEASURED"));
    KBNL_EXPECT_EQ(res_busy.execution_status, std::string("MEASURED"));
    KBNL_EXPECT_FALSE(res_blocking.is_simulated);
    KBNL_EXPECT_FALSE(res_busy.is_simulated);

    // Identical workload sent
    KBNL_EXPECT_EQ(res_blocking.actual_packets_sent, 1100ULL);
    KBNL_EXPECT_EQ(res_busy.actual_packets_sent, 1100ULL);

    // Exact sequence accounting verified on both paths
    KBNL_EXPECT_TRUE(res_blocking.sequence_accounting_verified);
    KBNL_EXPECT_TRUE(res_busy.sequence_accounting_verified);
    KBNL_EXPECT_EQ(res_blocking.sequence.decode_errors, 0ULL);
    KBNL_EXPECT_EQ(res_busy.sequence.decode_errors, 0ULL);

    // On local loopback at 20k pps, expect high delivery (>= 98%) and valid latency quantiles
    KBNL_EXPECT_TRUE(res_blocking.sequence.measured_received >= 980ULL);
    KBNL_EXPECT_TRUE(res_busy.sequence.measured_received >= 980ULL);
    KBNL_EXPECT_TRUE(res_blocking.latency.count == res_blocking.sequence.measured_received);
    KBNL_EXPECT_TRUE(res_busy.latency.count == res_busy.sequence.measured_received);
    KBNL_EXPECT_TRUE(res_blocking.latency.p50_ns <= res_blocking.latency.p99_ns);
    KBNL_EXPECT_TRUE(res_blocking.latency.p99_ns <= res_blocking.latency.p999_ns);
    KBNL_EXPECT_TRUE(res_busy.latency.p50_ns <= res_busy.latency.p99_ns);
    KBNL_EXPECT_TRUE(res_busy.latency.p99_ns <= res_busy.latency.p999_ns);

    // Identical CPU policy recorded on both runs
    KBNL_EXPECT_EQ(res_blocking.sender_pin.requested_cpu, res_busy.sender_pin.requested_cpu);
    KBNL_EXPECT_EQ(res_blocking.receiver_pin.requested_cpu, res_busy.receiver_pin.requested_cpu);
    KBNL_EXPECT_EQ(res_blocking.sender_pin.mechanism, res_busy.sender_pin.mechanism);
}

KBNL_TEST(burst_mode_loopback_preserves_sequence_accounting) {
    const auto cfg = make_loopback_cfg(
        kbnl::TransportBackend::kPosixUdpBlocking, kbnl::PacingMode::kBurst);
    const auto res = kbnl::run_benchmark(cfg);

    KBNL_EXPECT_EQ(res.execution_status, std::string("MEASURED"));
    KBNL_EXPECT_TRUE(res.sequence_accounting_verified);
    KBNL_EXPECT_TRUE(res.sequence.burst_packets > 0);
    KBNL_EXPECT_EQ(res.sequence.decode_errors, 0ULL);
    KBNL_EXPECT_TRUE(res.sequence.unique_received >= 1000ULL);
}

KBNL_TEST(reproducible_config_hash_and_logical_counts_across_reruns) {
    const auto cfg = make_loopback_cfg(
        kbnl::TransportBackend::kPosixUdpBlocking, kbnl::PacingMode::kConstantRate);
    const auto r1 = kbnl::run_benchmark(cfg);
    const auto r2 = kbnl::run_benchmark(cfg);

    KBNL_EXPECT_EQ(r1.config_hash, r2.config_hash);
    KBNL_EXPECT_EQ(r1.run_id, r2.run_id);
    KBNL_EXPECT_EQ(r1.actual_packets_sent, 1100ULL);
    KBNL_EXPECT_EQ(r2.actual_packets_sent, 1100ULL);
    KBNL_EXPECT_EQ(r1.send_errors, 0ULL);
    KBNL_EXPECT_EQ(r2.send_errors, 0ULL);
    KBNL_EXPECT_TRUE(r1.sequence_accounting_verified);
    KBNL_EXPECT_TRUE(r2.sequence_accounting_verified);
}

} // namespace

int main() {
    return kbnl::test::run_all_tests("integration_loopback");
}
