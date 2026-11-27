#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kbnl {

struct PinResult {
    bool requested = false;
    bool applied = false;
    int requested_cpu = -1;
    std::string mechanism;
    std::string detail;
};

struct CpuNicTopology {
    uint32_t logical_cpus = 0;
    std::string os_name;
    std::string arch;
    std::vector<std::string> interfaces;
    bool supports_strict_thread_pinning = false;
    bool af_xdp_compiled = false;
    bool dpdk_compiled = false;
};

// Pins the calling thread to cpu_id (>= 0).
// On Linux, calls pthread_setaffinity_np.
// On macOS, strict core pinning is not exposed by Darwin kernel APIs; returns applied=false with explicit reason.
PinResult pin_current_thread(int cpu_id);

// Discovers host logical CPU count, OS, architecture, network interfaces, and compiled feature flags.
CpuNicTopology discover_topology();

} // namespace kbnl
