#include "kbnl/cpu_affinity.hpp"

#include <algorithm>
#include <ifaddrs.h>
#include <net/if.h>
#include <set>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

namespace kbnl {

PinResult pin_current_thread(int cpu_id) {
    PinResult res{};
    res.requested_cpu = cpu_id;
    if (cpu_id < 0) {
        res.requested = false;
        res.applied = false;
        res.mechanism = "none";
        res.detail = "Thread pinning not requested (cpu_id < 0).";
        return res;
    }

    res.requested = true;
    const unsigned int hw = std::thread::hardware_concurrency();
    if (hw > 0 && static_cast<unsigned int>(cpu_id) >= hw) {
        res.applied = false;
        res.mechanism = "validation";
        res.detail = "Requested cpu_id " + std::to_string(cpu_id) +
                     " exceeds logical CPU count (" + std::to_string(hw) + ").";
        return res;
    }

#if defined(__linux__)
    res.mechanism = "pthread_setaffinity_np";
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc == 0) {
        res.applied = true;
        res.detail = "Pinned thread strictly to Linux logical CPU " + std::to_string(cpu_id) + ".";
    } else {
        res.applied = false;
        res.detail = std::string("pthread_setaffinity_np failed: ") + std::strerror(rc);
    }
#elif defined(__APPLE__)
    // Darwin/macOS on Apple Silicon does not expose strict per-core pinning to userspace.
    // We report this transparently rather than claiming strict pinning succeeded.
    res.mechanism = "macos_unpinned_advisory";
    res.applied = false;
    res.detail = "macOS Darwin kernel does not support strict CPU core pinning via POSIX affinity; "
                 "requested cpu_id=" + std::to_string(cpu_id) + " recorded as advisory only.";
#else
    res.mechanism = "unsupported_os";
    res.applied = false;
    res.detail = "Thread pinning not implemented on this OS.";
#endif
    return res;
}

CpuNicTopology discover_topology() {
    CpuNicTopology topo{};
    topo.logical_cpus = std::thread::hardware_concurrency();

#if defined(__linux__)
    topo.os_name = "Linux";
    topo.supports_strict_thread_pinning = true;
#elif defined(__APPLE__)
    topo.os_name = "macOS";
    topo.supports_strict_thread_pinning = false;
#else
    topo.os_name = "Other";
    topo.supports_strict_thread_pinning = false;
#endif

#if defined(__aarch64__) || defined(__arm64__)
    topo.arch = "arm64";
#elif defined(__x86_64__)
    topo.arch = "x86_64";
#else
    topo.arch = "unknown";
#endif

#if defined(KBNL_ENABLE_AF_XDP)
    topo.af_xdp_compiled = true;
#else
    topo.af_xdp_compiled = false;
#endif

#if defined(KBNL_ENABLE_DPDK)
    topo.dpdk_compiled = true;
#else
    topo.dpdk_compiled = false;
#endif

    ifaddrs* ifap = nullptr;
    if (::getifaddrs(&ifap) == 0 && ifap != nullptr) {
        std::set<std::string> unique_ifaces;
        for (ifaddrs* p = ifap; p != nullptr; p = p->ifa_next) {
            if (p->ifa_name != nullptr) {
                unique_ifaces.insert(p->ifa_name);
            }
        }
        ::freeifaddrs(ifap);
        topo.interfaces.assign(unique_ifaces.begin(), unique_ifaces.end());
    }

    return topo;
}

} // namespace kbnl
