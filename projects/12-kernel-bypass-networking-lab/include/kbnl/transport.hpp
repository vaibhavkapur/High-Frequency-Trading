#pragma once

#include "kbnl/config.hpp"
#include "kbnl/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kbnl {

enum class StatusCode {
    kOk = 0,
    kInvalidConfig,
    kUnsupportedPlatform,
    kFeatureDisabled,
    kMissingCapability,
    kDeviceUnavailable,
    kBindFailed,
    kIoError,
    kTimeout
};

std::string_view status_code_name(StatusCode code) noexcept;

struct Status {
    StatusCode code = StatusCode::kOk;
    std::string message;

    bool ok() const noexcept { return code == StatusCode::kOk; }
    static Status Ok() { return Status{StatusCode::kOk, ""}; }
    static Status Error(StatusCode c, std::string msg) { return Status{c, std::move(msg)}; }
};

struct CapabilityReport {
    std::string backend;
    bool compiled = false;
    bool os_supported = false;
    bool runtime_available = false;
    std::string status; // "AVAILABLE" or "NOT_RUN"
    StatusCode reason_code = StatusCode::kOk;
    std::string reason;
};

struct ReceivedFrame {
    uint64_t recv_ts_ns = 0;
    uint32_t length = 0;
    std::array<uint8_t, kMaxPacketBytes> data{};
};

class PacketReceiver {
public:
    virtual ~PacketReceiver() = default;

    virtual std::string_view backend_label() const noexcept = 0;
    virtual Status open(const TopologyConfig& topology) = 0;
    virtual uint16_t bound_port() const noexcept = 0;

    // Polls up to max_frames into out_frames. Returns number of frames received,
    // or sets out_status to non-Ok on fatal I/O error (timeout/EAGAIN returns 0 with kOk).
    virtual size_t poll_batch(
        std::vector<ReceivedFrame>& out_frames,
        size_t max_frames,
        uint32_t timeout_ms,
        Status& out_status) = 0;

    virtual void close() = 0;
};

class PacketSender {
public:
    virtual ~PacketSender() = default;

    virtual Status open(const TopologyConfig& topology, uint16_t target_port) = 0;
    virtual Status send_packet(std::span<const uint8_t> bytes) = 0;
    virtual void close() = 0;
};

CapabilityReport probe_backend_capability(TransportBackend backend, const TopologyConfig& topology);
std::unique_ptr<PacketReceiver> create_receiver(TransportBackend backend, Status& out_status);

} // namespace kbnl
