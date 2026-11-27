#pragma once

#include "kbnl/transport.hpp"

#include <cstddef>
#include <cstdint>

namespace kbnl {

// AF_XDP zero-copy / copy-mode receive adapter behind KBNL_ENABLE_AF_XDP feature gate.
// On Linux with KBNL_ENABLE_AF_XDP=1 and CAP_NET_RAW/CAP_NET_ADMIN + valid NIC interface,
// binds an AF_XDP socket with UMEM, fill ring, completion ring, and RX ring, and decodes
// UDP payloads from Ethernet/IPv4/UDP frames.
// On unsupported platforms or when unprivileged / disabled, fails explicitly with structured StatusCode.
class AfXdpReceiver final : public PacketReceiver {
public:
    AfXdpReceiver() = default;
    ~AfXdpReceiver() override;

    static CapabilityReport probe_capability(const TopologyConfig& topology);

    std::string_view backend_label() const noexcept override { return "af_xdp"; }
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
#if defined(__linux__) && defined(KBNL_ENABLE_AF_XDP)
    int xsk_fd_ = -1;
    void* umem_area_ = nullptr;
    size_t umem_size_ = 0;
    struct Impl;
    Impl* impl_ = nullptr;
#endif
};

} // namespace kbnl
