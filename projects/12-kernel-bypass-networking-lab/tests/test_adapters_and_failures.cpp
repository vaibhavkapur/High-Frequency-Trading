#include "test_framework.hpp"

#include "kbnl/af_xdp_transport.hpp"
#include "kbnl/benchmark_runner.hpp"
#include "kbnl/cpu_affinity.hpp"
#include "kbnl/dpdk_transport.hpp"
#include "kbnl/transport.hpp"

#include <string>

namespace {

KBNL_TEST(af_xdp_missing_hardware_or_privilege_fails_explicitly) {
    kbnl::TopologyConfig topo{};
    topo.interface_name = "eth_nonexistent_test0";
    topo.rx_queue_id = 0;

    const auto cap = kbnl::probe_backend_capability(kbnl::TransportBackend::kAfXdp, topo);
    KBNL_EXPECT_FALSE(cap.runtime_available);
    KBNL_EXPECT_EQ(cap.status, std::string("NOT_RUN"));
    KBNL_EXPECT_FALSE(cap.reason.empty());

    kbnl::AfXdpReceiver rx;
    const auto open_st = rx.open(topo);
    KBNL_EXPECT_FALSE(open_st.ok());
    KBNL_EXPECT_TRUE(
        open_st.code == kbnl::StatusCode::kUnsupportedPlatform ||
        open_st.code == kbnl::StatusCode::kFeatureDisabled ||
        open_st.code == kbnl::StatusCode::kMissingCapability ||
        open_st.code == kbnl::StatusCode::kDeviceUnavailable);

    // Running full benchmark with af_xdp must NOT silently succeed or simulate results.
    kbnl::BenchmarkConfig cfg{};
    cfg.workload.backend = kbnl::TransportBackend::kAfXdp;
    cfg.workload.measured_packets = 100;
    cfg.topology = topo;
    const auto res = kbnl::run_benchmark(cfg);
    KBNL_EXPECT_EQ(res.execution_status, std::string("NOT_RUN"));
    KBNL_EXPECT_FALSE(res.is_simulated);
    KBNL_EXPECT_FALSE(res.status.ok());
}

KBNL_TEST(dpdk_missing_hardware_or_privilege_fails_explicitly) {
    kbnl::TopologyConfig topo{};
    topo.interface_name = "eth_nonexistent_test0";

    const auto cap = kbnl::probe_backend_capability(kbnl::TransportBackend::kDpdk, topo);
    KBNL_EXPECT_FALSE(cap.runtime_available);
    KBNL_EXPECT_EQ(cap.status, std::string("NOT_RUN"));
    KBNL_EXPECT_FALSE(cap.reason.empty());

    kbnl::DpdkReceiver rx;
    const auto open_st = rx.open(topo);
    KBNL_EXPECT_FALSE(open_st.ok());

    kbnl::BenchmarkConfig cfg{};
    cfg.workload.backend = kbnl::TransportBackend::kDpdk;
    cfg.workload.measured_packets = 100;
    cfg.topology = topo;
    const auto res = kbnl::run_benchmark(cfg);
    KBNL_EXPECT_EQ(res.execution_status, std::string("NOT_RUN"));
    KBNL_EXPECT_FALSE(res.is_simulated);
    KBNL_EXPECT_FALSE(res.status.ok());
}

KBNL_TEST(non_linux_platform_reports_recvmmsg_and_strict_pinning_explicitly) {
#if !defined(__linux__)
    kbnl::TopologyConfig topo{};
    const auto cap = kbnl::probe_backend_capability(kbnl::TransportBackend::kPosixUdpRecvmmsg, topo);
    KBNL_EXPECT_FALSE(cap.runtime_available);
    KBNL_EXPECT_EQ(static_cast<int>(cap.reason_code), static_cast<int>(kbnl::StatusCode::kUnsupportedPlatform));

    // Strict pinning requested on macOS must fail explicitly instead of silently reporting applied=true
    kbnl::BenchmarkConfig cfg{};
    cfg.workload.measured_packets = 50;
    cfg.workload.warmup_packets = 10;
    cfg.topology.pin_threads = true;
    cfg.topology.strict_pinning = true;
    cfg.topology.sender_cpu = 0;
    cfg.topology.receiver_cpu = 1;
    const auto res = kbnl::run_benchmark(cfg);
    KBNL_EXPECT_EQ(res.execution_status, std::string("FAILED"));
    KBNL_EXPECT_EQ(static_cast<int>(res.status.code), static_cast<int>(kbnl::StatusCode::kUnsupportedPlatform));
#else
    // On Linux, out-of-range CPU pinning must fail validation cleanly
    const auto pin = kbnl::pin_current_thread(99999);
    KBNL_EXPECT_FALSE(pin.applied);
#endif
}

KBNL_TEST(invalid_config_rejected_by_benchmark_runner) {
    kbnl::BenchmarkConfig cfg{};
    cfg.workload.packet_bytes = 16; // Below minimum 64 bytes
    const auto res = kbnl::run_benchmark(cfg);
    KBNL_EXPECT_EQ(res.execution_status, std::string("FAILED"));
    KBNL_EXPECT_EQ(static_cast<int>(res.status.code), static_cast<int>(kbnl::StatusCode::kInvalidConfig));
}

KBNL_TEST(missing_fixture_file_fails_explicitly) {
    const auto res = kbnl::run_fixture_replay("/nonexistent/path/fixture.bin");
    KBNL_EXPECT_EQ(res.execution_status, std::string("FAILED"));
    KBNL_EXPECT_FALSE(res.status.ok());
}

} // namespace

int main() {
    return kbnl::test::run_all_tests("adapters_and_failures");
}
