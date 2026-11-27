#include "kbnl/af_xdp_transport.hpp"
#include "kbnl/clock.hpp"

#include <cstring>

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <net/if.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(KBNL_ENABLE_AF_XDP)
#include <atomic>
#include <linux/if_ether.h>
#include <linux/if_xdp.h>
#include <linux/ip.h>
#include <linux/udp.h>
#endif
#endif

namespace kbnl {

#if defined(__linux__) && defined(KBNL_ENABLE_AF_XDP)

struct AfXdpReceiver::Impl {
    static constexpr uint32_t kNumFrames = 4096;
    static constexpr uint32_t kFrameSize = 2048;
    static constexpr uint32_t kRingSize = 2048;

    void* fill_map = nullptr;
    size_t fill_map_size = 0;
    void* comp_map = nullptr;
    size_t comp_map_size = 0;
    void* rx_map = nullptr;
    size_t rx_map_size = 0;

    uint32_t* fill_prod = nullptr;
    uint32_t* fill_cons = nullptr;
    uint64_t* fill_ring = nullptr;

    uint32_t* rx_prod = nullptr;
    uint32_t* rx_cons = nullptr;
    xdp_desc* rx_ring = nullptr;

    uint32_t cached_fill_prod = 0;
    uint32_t cached_rx_cons = 0;
};

namespace {

bool extract_udp_payload(
    const uint8_t* frame,
    size_t frame_len,
    uint16_t target_port,
    const uint8_t*& out_payload,
    size_t& out_len) noexcept {
    if (frame_len < sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr)) {
        return false;
    }
    const auto* eth = reinterpret_cast<const ethhdr*>(frame);
    if (ntohs(eth->h_proto) != ETH_P_IP) {
        return false;
    }
    const auto* ip = reinterpret_cast<const iphdr*>(frame + sizeof(ethhdr));
    const size_t ihl = static_cast<size_t>(ip->ihl) * 4;
    if (ip->version != 4 || ihl < sizeof(iphdr) || ip->protocol != IPPROTO_UDP) {
        return false;
    }
    if (frame_len < sizeof(ethhdr) + ihl + sizeof(udphdr)) {
        return false;
    }
    const auto* udp = reinterpret_cast<const udphdr*>(frame + sizeof(ethhdr) + ihl);
    if (target_port != 0 && ntohs(udp->dest) != target_port) {
        return false;
    }
    const size_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(udphdr)) {
        return false;
    }
    const size_t payload_offset = sizeof(ethhdr) + ihl + sizeof(udphdr);
    const size_t payload_len = udp_len - sizeof(udphdr);
    if (payload_offset + payload_len > frame_len) {
        return false;
    }
    out_payload = frame + payload_offset;
    out_len = payload_len;
    return true;
}

} // namespace

#endif

AfXdpReceiver::~AfXdpReceiver() {
    close();
}

CapabilityReport AfXdpReceiver::probe_capability(const TopologyConfig& topology) {
    CapabilityReport rep{};
    rep.backend = "af_xdp";
#if !defined(__linux__)
    (void)topology;
    rep.compiled = false;
    rep.os_supported = false;
    rep.runtime_available = false;
    rep.status = "NOT_RUN";
    rep.reason_code = StatusCode::kUnsupportedPlatform;
    rep.reason = "AF_XDP requires a Linux kernel (>= 4.18) with XDP/BPF support; host OS is non-Linux.";
    return rep;
#elif !defined(KBNL_ENABLE_AF_XDP)
    (void)topology;
    rep.compiled = false;
    rep.os_supported = true;
    rep.runtime_available = false;
    rep.status = "NOT_RUN";
    rep.reason_code = StatusCode::kFeatureDisabled;
    rep.reason = "AF_XDP adapter not compiled (reconfigure CMake with -DKBNL_ENABLE_AF_XDP=ON on Linux).";
    return rep;
#else
    rep.compiled = true;
    rep.os_supported = true;
    if (::geteuid() != 0) {
        rep.runtime_available = false;
        rep.status = "NOT_RUN";
        rep.reason_code = StatusCode::kMissingCapability;
        rep.reason = "AF_XDP requires root or CAP_NET_RAW/CAP_NET_ADMIN/CAP_BPF capabilities.";
        return rep;
    }
    const unsigned int ifindex = ::if_nametoindex(topology.interface_name.c_str());
    if (ifindex == 0) {
        rep.runtime_available = false;
        rep.status = "NOT_RUN";
        rep.reason_code = StatusCode::kDeviceUnavailable;
        rep.reason = "Network interface '" + topology.interface_name + "' does not exist.";
        return rep;
    }
    const int probe_fd = ::socket(AF_XDP, SOCK_RAW, 0);
    if (probe_fd < 0) {
        rep.runtime_available = false;
        rep.status = "NOT_RUN";
        rep.reason_code = (errno == EPERM || errno == EACCES)
                              ? StatusCode::kMissingCapability
                              : StatusCode::kDeviceUnavailable;
        rep.reason = std::string("socket(AF_XDP, SOCK_RAW, 0) probe failed: ") + std::strerror(errno);
        return rep;
    }
    ::close(probe_fd);
    rep.runtime_available = true;
    rep.status = "AVAILABLE";
    rep.reason_code = StatusCode::kOk;
    rep.reason = "AF_XDP socket creation succeeded on interface '" + topology.interface_name + "'.";
    return rep;
#endif
}

Status AfXdpReceiver::open(const TopologyConfig& topology) {
    close();
    const CapabilityReport cap = probe_capability(topology);
    if (!cap.runtime_available) {
        return Status::Error(cap.reason_code, cap.reason);
    }

#if defined(__linux__) && defined(KBNL_ENABLE_AF_XDP)
    target_port_ = topology.port;
    const unsigned int ifindex = ::if_nametoindex(topology.interface_name.c_str());
    if (ifindex == 0) {
        return Status::Error(StatusCode::kDeviceUnavailable,
                             "if_nametoindex failed for interface: " + topology.interface_name);
    }

    xsk_fd_ = ::socket(AF_XDP, SOCK_RAW, 0);
    if (xsk_fd_ < 0) {
        return Status::Error(StatusCode::kMissingCapability,
                             std::string("socket(AF_XDP) failed: ") + std::strerror(errno));
    }

    impl_ = new Impl();
    umem_size_ = static_cast<size_t>(Impl::kNumFrames) * Impl::kFrameSize;
    umem_area_ = ::mmap(nullptr, umem_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (umem_area_ == MAP_FAILED) {
        umem_area_ = nullptr;
        close();
        return Status::Error(StatusCode::kIoError, "mmap UMEM failed");
    }

    xdp_umem_reg mr{};
    mr.addr = reinterpret_cast<uint64_t>(umem_area_);
    mr.len = umem_size_;
    mr.chunk_size = Impl::kFrameSize;
    mr.headroom = 0;
    if (::setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr)) != 0) {
        const std::string err = std::strerror(errno);
        close();
        return Status::Error(StatusCode::kIoError, "setsockopt(XDP_UMEM_REG) failed: " + err);
    }

    uint32_t ring_sz = Impl::kRingSize;
    if (::setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_FILL_RING, &ring_sz, sizeof(ring_sz)) != 0 ||
        ::setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_sz, sizeof(ring_sz)) != 0 ||
        ::setsockopt(xsk_fd_, SOL_XDP, XDP_RX_RING, &ring_sz, sizeof(ring_sz)) != 0) {
        const std::string err = std::strerror(errno);
        close();
        return Status::Error(StatusCode::kIoError, "setsockopt(XDP_*_RING) failed: " + err);
    }

    xdp_mmap_offsets off{};
    socklen_t optlen = sizeof(off);
    if (::getsockopt(xsk_fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) != 0) {
        const std::string err = std::strerror(errno);
        close();
        return Status::Error(StatusCode::kIoError, "getsockopt(XDP_MMAP_OFFSETS) failed: " + err);
    }

    impl_->fill_map_size = off.fr.desc + static_cast<size_t>(Impl::kRingSize) * sizeof(uint64_t);
    impl_->fill_map = ::mmap(nullptr, impl_->fill_map_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, xsk_fd_, XDP_UMEM_PGOFF_FILL_RING);
    impl_->rx_map_size = off.rx.desc + static_cast<size_t>(Impl::kRingSize) * sizeof(xdp_desc);
    impl_->rx_map = ::mmap(nullptr, impl_->rx_map_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, xsk_fd_, XDP_PGOFF_RX_RING);
    if (impl_->fill_map == MAP_FAILED || impl_->rx_map == MAP_FAILED) {
        close();
        return Status::Error(StatusCode::kIoError, "mmap AF_XDP rings failed");
    }

    auto* fill_base = static_cast<uint8_t*>(impl_->fill_map);
    impl_->fill_prod = reinterpret_cast<uint32_t*>(fill_base + off.fr.producer);
    impl_->fill_cons = reinterpret_cast<uint32_t*>(fill_base + off.fr.consumer);
    impl_->fill_ring = reinterpret_cast<uint64_t*>(fill_base + off.fr.desc);

    auto* rx_base = static_cast<uint8_t*>(impl_->rx_map);
    impl_->rx_prod = reinterpret_cast<uint32_t*>(rx_base + off.rx.producer);
    impl_->rx_cons = reinterpret_cast<uint32_t*>(rx_base + off.rx.consumer);
    impl_->rx_ring = reinterpret_cast<xdp_desc*>(rx_base + off.rx.desc);

    // Populate fill ring with initial UMEM frame descriptors.
    for (uint32_t i = 0; i < Impl::kRingSize; ++i) {
        impl_->fill_ring[i] = static_cast<uint64_t>(i) * Impl::kFrameSize;
    }
    impl_->cached_fill_prod = Impl::kRingSize;
    std::atomic_thread_fence(std::memory_order_release);
    *impl_->fill_prod = impl_->cached_fill_prod;

    sockaddr_xdp sxdp{};
    sxdp.sxdp_family = PF_XDP;
    sxdp.sxdp_ifindex = ifindex;
    sxdp.sxdp_queue_id = topology.rx_queue_id;
    sxdp.sxdp_flags = XDP_USE_NEED_WAKEUP;

    if (::bind(xsk_fd_, reinterpret_cast<const sockaddr*>(&sxdp), sizeof(sxdp)) != 0) {
        const std::string err = std::strerror(errno);
        close();
        return Status::Error(StatusCode::kBindFailed,
                             "bind(AF_XDP) failed on " + topology.interface_name +
                                 " queue " + std::to_string(topology.rx_queue_id) + ": " + err);
    }

    open_ = true;
    return Status::Ok();
#else
    return Status::Error(cap.reason_code, cap.reason);
#endif
}

size_t AfXdpReceiver::poll_batch(
    std::vector<ReceivedFrame>& out_frames,
    size_t max_frames,
    uint32_t timeout_ms,
    Status& out_status) {
    out_frames.clear();
#if defined(__linux__) && defined(KBNL_ENABLE_AF_XDP)
    if (!open_ || impl_ == nullptr) {
        out_status = Status::Error(StatusCode::kIoError, "AF_XDP receiver is not open");
        return 0;
    }
    out_status = Status::Ok();

    uint32_t rx_prod = *impl_->rx_prod;
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t avail = rx_prod - impl_->cached_rx_cons;
    if (avail == 0) {
        pollfd pfd{};
        pfd.fd = xsk_fd_;
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (prc <= 0) {
            return 0;
        }
        rx_prod = *impl_->rx_prod;
        std::atomic_thread_fence(std::memory_order_acquire);
        avail = rx_prod - impl_->cached_rx_cons;
    }

    const size_t to_read = std::min<size_t>(avail, max_frames);
    const uint64_t ts = now_ns();
    for (size_t i = 0; i < to_read; ++i) {
        const uint32_t idx = (impl_->cached_rx_cons + static_cast<uint32_t>(i)) & (Impl::kRingSize - 1);
        const xdp_desc desc = impl_->rx_ring[idx];
        const auto* frame_ptr = static_cast<const uint8_t*>(umem_area_) + desc.addr;

        const uint8_t* payload = nullptr;
        size_t payload_len = 0;
        if (extract_udp_payload(frame_ptr, desc.len, target_port_, payload, payload_len) &&
            payload_len <= kMaxPacketBytes) {
            ReceivedFrame rf{};
            rf.recv_ts_ns = ts;
            rf.length = static_cast<uint32_t>(payload_len);
            std::memcpy(rf.data.data(), payload, payload_len);
            out_frames.push_back(rf);
        }

        // Recycle frame address back to fill ring.
        const uint32_t fill_idx = impl_->cached_fill_prod & (Impl::kRingSize - 1);
        impl_->fill_ring[fill_idx] = desc.addr;
        ++impl_->cached_fill_prod;
    }

    impl_->cached_rx_cons += static_cast<uint32_t>(to_read);
    std::atomic_thread_fence(std::memory_order_release);
    *impl_->rx_cons = impl_->cached_rx_cons;
    *impl_->fill_prod = impl_->cached_fill_prod;
    return out_frames.size();
#else
    (void)max_frames;
    (void)timeout_ms;
    out_status = Status::Error(StatusCode::kUnsupportedPlatform,
                               "AF_XDP receiver is unavailable on this platform/build.");
    return 0;
#endif
}

void AfXdpReceiver::close() {
#if defined(__linux__) && defined(KBNL_ENABLE_AF_XDP)
    if (impl_ != nullptr) {
        if (impl_->fill_map != nullptr && impl_->fill_map != MAP_FAILED) {
            ::munmap(impl_->fill_map, impl_->fill_map_size);
        }
        if (impl_->rx_map != nullptr && impl_->rx_map != MAP_FAILED) {
            ::munmap(impl_->rx_map, impl_->rx_map_size);
        }
        delete impl_;
        impl_ = nullptr;
    }
    if (umem_area_ != nullptr && umem_area_ != MAP_FAILED) {
        ::munmap(umem_area_, umem_size_);
        umem_area_ = nullptr;
        umem_size_ = 0;
    }
    if (xsk_fd_ >= 0) {
        ::close(xsk_fd_);
        xsk_fd_ = -1;
    }
#endif
    open_ = false;
}

} // namespace kbnl
