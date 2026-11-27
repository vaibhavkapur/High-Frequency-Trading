#include "kbnl/dpdk_transport.hpp"
#include "kbnl/clock.hpp"

#include <cstring>

#if defined(__linux__) && defined(KBNL_ENABLE_DPDK)
#include <arpa/inet.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <unistd.h>
#endif

namespace kbnl {

DpdkReceiver::~DpdkReceiver() {
    close();
}

CapabilityReport DpdkReceiver::probe_capability(const TopologyConfig& topology) {
    (void)topology;
    CapabilityReport rep{};
    rep.backend = "dpdk";
#if !defined(__linux__)
    rep.compiled = false;
    rep.os_supported = false;
    rep.runtime_available = false;
    rep.status = "NOT_RUN";
    rep.reason_code = StatusCode::kUnsupportedPlatform;
    rep.reason = "DPDK user-space PMD path requires Linux with hugepages and VFIO/UIO NIC binding; host OS is non-Linux.";
    return rep;
#elif !defined(KBNL_ENABLE_DPDK)
    rep.compiled = false;
    rep.os_supported = true;
    rep.runtime_available = false;
    rep.status = "NOT_RUN";
    rep.reason_code = StatusCode::kFeatureDisabled;
    rep.reason = "DPDK adapter not compiled (reconfigure CMake with -DKBNL_ENABLE_DPDK=ON with libdpdk installed).";
    return rep;
#else
    rep.compiled = true;
    rep.os_supported = true;
    if (::geteuid() != 0) {
        rep.runtime_available = false;
        rep.status = "NOT_RUN";
        rep.reason_code = StatusCode::kMissingCapability;
        rep.reason = "DPDK EAL requires privileged access to hugepages and VFIO/UIO devices.";
        return rep;
    }
    rep.runtime_available = true;
    rep.status = "AVAILABLE";
    rep.reason_code = StatusCode::kOk;
    rep.reason = "DPDK compiled and privileged execution detected; port availability checked on open().";
    return rep;
#endif
}

Status DpdkReceiver::open(const TopologyConfig& topology) {
    close();
    const CapabilityReport cap = probe_capability(topology);
    if (!cap.runtime_available) {
        return Status::Error(cap.reason_code, cap.reason);
    }

#if defined(__linux__) && defined(KBNL_ENABLE_DPDK)
    target_port_ = topology.port;
    rx_queue_id_ = static_cast<uint16_t>(topology.rx_queue_id);

    static bool eal_initialized = false;
    if (!eal_initialized) {
        char prog[] = "kbnl_dpdk";
        char* argv[] = {prog, nullptr};
        if (rte_eal_init(1, argv) < 0) {
            return Status::Error(StatusCode::kDeviceUnavailable, "rte_eal_init() failed (check hugepages/VFIO).");
        }
        eal_initialized = true;
    }

    const uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        return Status::Error(StatusCode::kDeviceUnavailable, "No DPDK-bound Ethernet ports available.");
    }
    port_id_ = 0;

    auto* pool = rte_pktmbuf_pool_create(
        "KBNL_MBUF_POOL",
        8191,
        256,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id());
    if (pool == nullptr) {
        return Status::Error(StatusCode::kIoError, "rte_pktmbuf_pool_create() failed.");
    }
    mbuf_pool_ = pool;

    rte_eth_conf port_conf{};
    if (rte_eth_dev_configure(port_id_, 1, 0, &port_conf) != 0) {
        return Status::Error(StatusCode::kIoError, "rte_eth_dev_configure() failed.");
    }
    if (rte_eth_rx_queue_setup(port_id_, rx_queue_id_, 1024, rte_eth_dev_socket_id(port_id_), nullptr, pool) != 0) {
        return Status::Error(StatusCode::kIoError, "rte_eth_rx_queue_setup() failed.");
    }
    if (rte_eth_dev_start(port_id_) != 0) {
        return Status::Error(StatusCode::kIoError, "rte_eth_dev_start() failed.");
    }

    open_ = true;
    return Status::Ok();
#else
    return Status::Error(cap.reason_code, cap.reason);
#endif
}

size_t DpdkReceiver::poll_batch(
    std::vector<ReceivedFrame>& out_frames,
    size_t max_frames,
    uint32_t timeout_ms,
    Status& out_status) {
    out_frames.clear();
#if defined(__linux__) && defined(KBNL_ENABLE_DPDK)
    if (!open_) {
        out_status = Status::Error(StatusCode::kIoError, "DPDK receiver is not open");
        return 0;
    }
    out_status = Status::Ok();
    const uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL;
    const uint16_t burst_cap = static_cast<uint16_t>(std::min<size_t>(max_frames, 64));
    rte_mbuf* bufs[64];

    while (true) {
        const uint16_t nb_rx = rte_eth_rx_burst(port_id_, rx_queue_id_, bufs, burst_cap);
        if (nb_rx > 0) {
            const uint64_t ts = now_ns();
            for (uint16_t i = 0; i < nb_rx; ++i) {
                const auto* data = rte_pktmbuf_mtod(bufs[i], const uint8_t*);
                const size_t pkt_len = rte_pktmbuf_pkt_len(bufs[i]);
                if (pkt_len >= sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr)) {
                    const auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
                    if (ntohs(eth->ether_type) == RTE_ETHER_TYPE_IPV4) {
                        const auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + sizeof(rte_ether_hdr));
                        const size_t ihl = static_cast<size_t>(ip->version_ihl & 0x0Fu) * 4;
                        if (ip->next_proto_id == IPPROTO_UDP &&
                            pkt_len >= sizeof(rte_ether_hdr) + ihl + sizeof(rte_udp_hdr)) {
                            const auto* udp = reinterpret_cast<const rte_udp_hdr*>(data + sizeof(rte_ether_hdr) + ihl);
                            if (target_port_ == 0 || ntohs(udp->dst_port) == target_port_) {
                                const size_t payload_off = sizeof(rte_ether_hdr) + ihl + sizeof(rte_udp_hdr);
                                const size_t udp_len = ntohs(udp->dgram_len);
                                if (udp_len >= sizeof(rte_udp_hdr) && payload_off + udp_len - sizeof(rte_udp_hdr) <= pkt_len) {
                                    const size_t payload_len = udp_len - sizeof(rte_udp_hdr);
                                    if (payload_len <= kMaxPacketBytes) {
                                        ReceivedFrame rf{};
                                        rf.recv_ts_ns = ts;
                                        rf.length = static_cast<uint32_t>(payload_len);
                                        std::memcpy(rf.data.data(), data + payload_off, payload_len);
                                        out_frames.push_back(rf);
                                    }
                                }
                            }
                        }
                    }
                }
                rte_pktmbuf_free(bufs[i]);
            }
            return out_frames.size();
        }
        if (now_ns() >= deadline) {
            return 0;
        }
    }
#else
    (void)max_frames;
    (void)timeout_ms;
    out_status = Status::Error(StatusCode::kUnsupportedPlatform,
                               "DPDK receiver is unavailable on this platform/build.");
    return 0;
#endif
}

void DpdkReceiver::close() {
#if defined(__linux__) && defined(KBNL_ENABLE_DPDK)
    if (open_) {
        rte_eth_dev_stop(port_id_);
    }
#endif
    open_ = false;
}

} // namespace kbnl
