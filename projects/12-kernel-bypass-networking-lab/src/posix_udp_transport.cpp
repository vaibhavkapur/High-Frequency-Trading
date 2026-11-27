#include "kbnl/posix_udp_transport.hpp"
#include "kbnl/af_xdp_transport.hpp"
#include "kbnl/clock.hpp"
#include "kbnl/dpdk_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kbnl {

std::string_view status_code_name(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::kOk: return "OK";
        case StatusCode::kInvalidConfig: return "INVALID_CONFIG";
        case StatusCode::kUnsupportedPlatform: return "UNSUPPORTED_PLATFORM";
        case StatusCode::kFeatureDisabled: return "FEATURE_DISABLED";
        case StatusCode::kMissingCapability: return "MISSING_CAPABILITY";
        case StatusCode::kDeviceUnavailable: return "DEVICE_UNAVAILABLE";
        case StatusCode::kBindFailed: return "BIND_FAILED";
        case StatusCode::kIoError: return "IO_ERROR";
        case StatusCode::kTimeout: return "TIMEOUT";
    }
    return "UNKNOWN";
}

PosixUdpReceiver::PosixUdpReceiver(PosixReceiveMode mode) : mode_(mode) {}

PosixUdpReceiver::~PosixUdpReceiver() {
    close();
}

std::string_view PosixUdpReceiver::backend_label() const noexcept {
    switch (mode_) {
        case PosixReceiveMode::kBlockingTimeout: return "posix_udp_blocking";
        case PosixReceiveMode::kBusyPollNonBlocking: return "posix_udp_busy_poll";
        case PosixReceiveMode::kRecvmmsgBatched: return "posix_udp_recvmmsg";
    }
    return "posix_udp_blocking";
}

Status PosixUdpReceiver::open(const TopologyConfig& topology) {
    close();

#if !defined(__linux__)
    if (mode_ == PosixReceiveMode::kRecvmmsgBatched) {
        return Status::Error(
            StatusCode::kUnsupportedPlatform,
            "recvmmsg() batched receive path is Linux-only and unavailable on this OS.");
    }
#endif

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return Status::Error(StatusCode::kIoError, std::string("socket() failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int rcvbuf = static_cast<int>(topology.socket_rcvbuf_bytes);
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int actual = 0;
    socklen_t actual_len = sizeof(actual);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &actual, &actual_len) == 0 && actual > 0) {
        actual_rcvbuf_bytes_ = static_cast<uint32_t>(actual);
    }

    // Put socket in non-blocking mode; blocking mode uses poll() for bounded waits.
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(topology.port);
    if (::inet_pton(AF_INET, topology.bind_ip.c_str(), &addr.sin_addr) != 1) {
        close();
        return Status::Error(StatusCode::kInvalidConfig, "Invalid bind_ip: " + topology.bind_ip);
    }

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string err = std::strerror(errno);
        close();
        return Status::Error(StatusCode::kBindFailed, "bind() failed on " + topology.bind_ip + ": " + err);
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = topology.port;
    }

    return Status::Ok();
}

size_t PosixUdpReceiver::poll_batch(
    std::vector<ReceivedFrame>& out_frames,
    size_t max_frames,
    uint32_t timeout_ms,
    Status& out_status) {
    out_status = Status::Ok();
    out_frames.clear();
    if (fd_ < 0 || max_frames == 0) {
        return 0;
    }

    if (mode_ == PosixReceiveMode::kBlockingTimeout) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (prc < 0) {
            if (errno == EINTR) {
                return 0;
            }
            out_status = Status::Error(StatusCode::kIoError, std::string("poll() failed: ") + std::strerror(errno));
            return 0;
        }
        if (prc == 0) {
            return 0;
        }
        while (out_frames.size() < max_frames) {
            ReceivedFrame frame{};
            const ssize_t n = ::recvfrom(fd_, frame.data.data(), frame.data.size(), MSG_DONTWAIT, nullptr, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    break;
                }
                out_status = Status::Error(StatusCode::kIoError, std::string("recvfrom() failed: ") + std::strerror(errno));
                return out_frames.size();
            }
            frame.recv_ts_ns = now_ns();
            frame.length = static_cast<uint32_t>(n);
            out_frames.push_back(frame);
        }
        return out_frames.size();
    }

    if (mode_ == PosixReceiveMode::kBusyPollNonBlocking) {
        const uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL;
        while (out_frames.size() < max_frames) {
            ReceivedFrame frame{};
            const ssize_t n = ::recvfrom(fd_, frame.data.data(), frame.data.size(), MSG_DONTWAIT, nullptr, nullptr);
            if (n >= 0) {
                frame.recv_ts_ns = now_ns();
                frame.length = static_cast<uint32_t>(n);
                out_frames.push_back(frame);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (!out_frames.empty()) {
                    // Return immediately once a burst has been drained.
                    break;
                }
                if (now_ns() >= deadline) {
                    break;
                }
                continue;
            }
            out_status = Status::Error(StatusCode::kIoError, std::string("recvfrom() failed: ") + std::strerror(errno));
            return out_frames.size();
        }
        return out_frames.size();
    }

#if defined(__linux__)
    // Linux recvmmsg() batched syscall path.
    const size_t batch = std::min<size_t>(max_frames, 64);
    out_frames.resize(batch);
    std::vector<iovec> iovs(batch);
    std::vector<mmsghdr> msgs(batch);
    std::memset(msgs.data(), 0, sizeof(mmsghdr) * batch);
    for (size_t i = 0; i < batch; ++i) {
        iovs[i].iov_base = out_frames[i].data.data();
        iovs[i].iov_len = out_frames[i].data.size();
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int prc = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (prc <= 0) {
        out_frames.clear();
        if (prc < 0 && errno != EINTR) {
            out_status = Status::Error(StatusCode::kIoError, std::string("poll() failed: ") + std::strerror(errno));
        }
        return 0;
    }

    const int count = ::recvmmsg(fd_, msgs.data(), static_cast<unsigned int>(batch), MSG_DONTWAIT, nullptr);
    const uint64_t ts = now_ns();
    if (count < 0) {
        out_frames.clear();
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            out_status = Status::Error(StatusCode::kIoError, std::string("recvmmsg() failed: ") + std::strerror(errno));
        }
        return 0;
    }
    out_frames.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out_frames[static_cast<size_t>(i)].recv_ts_ns = ts;
        out_frames[static_cast<size_t>(i)].length = msgs[static_cast<size_t>(i)].msg_len;
    }
    return static_cast<size_t>(count);
#else
    out_status = Status::Error(
        StatusCode::kUnsupportedPlatform,
        "recvmmsg() batched receive path is Linux-only.");
    return 0;
#endif
}

void PosixUdpReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    bound_port_ = 0;
}

PosixUdpSender::~PosixUdpSender() {
    close();
}

Status PosixUdpSender::open(const TopologyConfig& topology, uint16_t target_port) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return Status::Error(StatusCode::kIoError, std::string("sender socket() failed: ") + std::strerror(errno));
    }

    int sndbuf = static_cast<int>(topology.socket_sndbuf_bytes);
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int actual = 0;
    socklen_t actual_len = sizeof(actual);
    if (::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &actual, &actual_len) == 0 && actual > 0) {
        actual_sndbuf_bytes_ = static_cast<uint32_t>(actual);
    }

    std::memset(&target_addr_, 0, sizeof(target_addr_));
    target_addr_.sin_family = AF_INET;
    target_addr_.sin_port = htons(target_port);
    if (::inet_pton(AF_INET, topology.target_ip.c_str(), &target_addr_.sin_addr) != 1) {
        close();
        return Status::Error(StatusCode::kInvalidConfig, "Invalid target_ip: " + topology.target_ip);
    }

    return Status::Ok();
}

Status PosixUdpSender::send_packet(std::span<const uint8_t> bytes) {
    if (fd_ < 0) {
        return Status::Error(StatusCode::kIoError, "Sender socket is not open");
    }
    const ssize_t sent = ::sendto(
        fd_,
        bytes.data(),
        bytes.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target_addr_),
        sizeof(target_addr_));
    if (sent < 0) {
        return Status::Error(StatusCode::kIoError, std::string("sendto() failed: ") + std::strerror(errno));
    }
    if (static_cast<size_t>(sent) != bytes.size()) {
        return Status::Error(StatusCode::kIoError, "Partial UDP datagram send");
    }
    return Status::Ok();
}

void PosixUdpSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CapabilityReport probe_backend_capability(TransportBackend backend, const TopologyConfig& topology) {
    switch (backend) {
        case TransportBackend::kPosixUdpBlocking:
            return CapabilityReport{
                "posix_udp_blocking",
                true,
                true,
                true,
                "AVAILABLE",
                StatusCode::kOk,
                "Standard POSIX UDP socket with poll() + non-blocking drain."
            };
        case TransportBackend::kPosixUdpBusyPoll:
            return CapabilityReport{
                "posix_udp_busy_poll",
                true,
                true,
                true,
                "AVAILABLE",
                StatusCode::kOk,
                "Standard POSIX UDP non-blocking busy-poll loop (no kernel sleep between arrivals)."
            };
        case TransportBackend::kPosixUdpRecvmmsg:
#if defined(__linux__)
            return CapabilityReport{
                "posix_udp_recvmmsg",
                true,
                true,
                true,
                "AVAILABLE",
                StatusCode::kOk,
                "Linux recvmmsg() batched UDP socket receive path."
            };
#else
            return CapabilityReport{
                "posix_udp_recvmmsg",
                false,
                false,
                false,
                "NOT_RUN",
                StatusCode::kUnsupportedPlatform,
                "recvmmsg() is a Linux-specific syscall and is unavailable on this OS."
            };
#endif
        case TransportBackend::kAfXdp:
            return AfXdpReceiver::probe_capability(topology);
        case TransportBackend::kDpdk:
            return DpdkReceiver::probe_capability(topology);
    }
    return CapabilityReport{
        "unknown",
        false,
        false,
        false,
        "NOT_RUN",
        StatusCode::kInvalidConfig,
        "Unknown backend."
    };
}

std::unique_ptr<PacketReceiver> create_receiver(TransportBackend backend, Status& out_status) {
    out_status = Status::Ok();
    switch (backend) {
        case TransportBackend::kPosixUdpBlocking:
            return std::make_unique<PosixUdpReceiver>(PosixReceiveMode::kBlockingTimeout);
        case TransportBackend::kPosixUdpBusyPoll:
            return std::make_unique<PosixUdpReceiver>(PosixReceiveMode::kBusyPollNonBlocking);
        case TransportBackend::kPosixUdpRecvmmsg:
            return std::make_unique<PosixUdpReceiver>(PosixReceiveMode::kRecvmmsgBatched);
        case TransportBackend::kAfXdp:
            return std::make_unique<AfXdpReceiver>();
        case TransportBackend::kDpdk:
            return std::make_unique<DpdkReceiver>();
    }
    out_status = Status::Error(StatusCode::kInvalidConfig, "Unsupported transport backend enum");
    return nullptr;
}

} // namespace kbnl
