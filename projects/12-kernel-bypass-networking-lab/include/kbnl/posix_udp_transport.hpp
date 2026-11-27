#pragma once

#include "kbnl/transport.hpp"

#include <netinet/in.h>

namespace kbnl {

enum class PosixReceiveMode {
    kBlockingTimeout = 0,
    kBusyPollNonBlocking,
    kRecvmmsgBatched
};

class PosixUdpReceiver final : public PacketReceiver {
public:
    explicit PosixUdpReceiver(PosixReceiveMode mode);
    ~PosixUdpReceiver() override;

    std::string_view backend_label() const noexcept override;
    Status open(const TopologyConfig& topology) override;
    uint16_t bound_port() const noexcept override { return bound_port_; }

    size_t poll_batch(
        std::vector<ReceivedFrame>& out_frames,
        size_t max_frames,
        uint32_t timeout_ms,
        Status& out_status) override;

    void close() override;

    uint32_t actual_rcvbuf_bytes() const noexcept { return actual_rcvbuf_bytes_; }

private:
    PosixReceiveMode mode_;
    int fd_ = -1;
    uint16_t bound_port_ = 0;
    uint32_t actual_rcvbuf_bytes_ = 0;
};

class PosixUdpSender final : public PacketSender {
public:
    PosixUdpSender() = default;
    ~PosixUdpSender() override;

    Status open(const TopologyConfig& topology, uint16_t target_port) override;
    Status send_packet(std::span<const uint8_t> bytes) override;
    void close() override;

    uint32_t actual_sndbuf_bytes() const noexcept { return actual_sndbuf_bytes_; }

private:
    int fd_ = -1;
    sockaddr_in target_addr_{};
    uint32_t actual_sndbuf_bytes_ = 0;
};

} // namespace kbnl
