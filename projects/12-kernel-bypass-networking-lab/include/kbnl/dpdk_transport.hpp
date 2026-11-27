#pragma once

#include "kbnl/transport.hpp"

#include <cstdint>

namespace kbnl {

// Optional DPDK receive adapter behind KBNL_ENABLE_DPDK feature gate.
// When compiled on Linux with libdpdk and bound VFIO/UIO NIC + hugepages, polls via rte_eth_rx_burst
// and strips Ethernet/IPv4/UDP headers matching target_port_.
// Otherwise returns explicit StatusCode::kUnsupportedPlatform / kFeatureDisabled / kMissingCapability.
class DpdkReceiver final : public PacketReceiver {
public:
    DpdkReceiver() = default;
    ~DpdkReceiver() override;

    static CapabilityReport probe_capability(const TopologyConfig& topology);

    std::string_view backend_label() const noexcept override { return "dpdk"; }
    Status open(const TopologyConfig& topology) override;
    uint16_t bound_port() const noexcept override { return target_port_; }

    size_t poll_batch(
        std::vector<ReceivedFrame>& out_frames,
        size_t max_frames,
        uint32_t timeout_ms,
        Status& out_status) override;

    void close() override;

private:
    uint16_t target_port_ = 0;
    bool open_ = false;
#if defined(__linux__) && defined(KBNL_ENABLE_DPDK)
    uint16_t port_id_ = 0;
    uint16_t rx_queue_id_ = 0;
    void* mbuf_pool_ = nullptr;
#endif
};

} // namespace kbnl
