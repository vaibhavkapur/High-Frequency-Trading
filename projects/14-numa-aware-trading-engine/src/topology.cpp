#include "numa_engine/topology.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "numa_engine/json_utils.hpp"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace numa_engine {

namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

#if defined(__APPLE__)
template <typename T>
bool read_sysctl(const char* name, T& out) {
    std::size_t size = sizeof(T);
    return sysctlbyname(name, &out, &size, nullptr, 0) == 0;
}

std::string read_sysctl_string(const char* name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::string buf(size, '\0');
    if (sysctlbyname(name, buf.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    while (!buf.empty() && buf.back() == '\0') {
        buf.pop_back();
    }
    return buf;
}
#endif

#if defined(__linux__)
std::string read_first_line(const std::filesystem::path& p) {
    std::ifstream ifs(p);
    std::string line;
    if (ifs && std::getline(ifs, line)) {
        return trim(line);
    }
    return {};
}

std::uint64_t parse_linux_meminfo_bytes(const std::filesystem::path& p) {
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("MemTotal:") != std::string::npos) {
            std::istringstream iss(line);
            std::string tok;
            while (iss >> tok) {
                if (tok == "MemTotal:") {
                    std::uint64_t kb = 0;
                    if (iss >> kb) {
                        return kb * 1024ULL;
                    }
                }
            }
        }
    }
    return 0;
}
#endif

}  // namespace

std::vector<int> parse_linux_cpulist(const std::string& cpulist) {
    std::vector<int> cpus;
    std::stringstream ss(trim(cpulist));
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = trim(token);
        if (token.empty()) continue;
        const auto dash = token.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(token));
        } else {
            const int start = std::stoi(token.substr(0, dash));
            const int end = std::stoi(token.substr(dash + 1));
            for (int c = start; c <= end; ++c) {
                cpus.push_back(c);
            }
        }
    }
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return cpus;
}

bool TopologyInfo::has_cpu(int cpu_id) const noexcept {
    if (cpu_id < 0) return false;
    for (const auto& node : nodes) {
        if (std::find(node.cpu_ids.begin(), node.cpu_ids.end(), cpu_id) != node.cpu_ids.end()) {
            return true;
        }
    }
    return static_cast<std::size_t>(cpu_id) < logical_cpus;
}

bool TopologyInfo::has_node(int node_id) const noexcept {
    if (node_id < 0) return false;
    for (const auto& node : nodes) {
        if (node.node_id == node_id) return true;
    }
    return false;
}

int TopologyInfo::node_for_cpu(int cpu_id) const noexcept {
    for (const auto& node : nodes) {
        if (std::find(node.cpu_ids.begin(), node.cpu_ids.end(), cpu_id) != node.cpu_ids.end()) {
            return node.node_id;
        }
    }
    return 0;
}

std::string TopologyInfo::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"os_name\":" << json::quote(os_name) << ","
        << "\"os_release\":" << json::quote(os_release) << ","
        << "\"arch\":" << json::quote(arch) << ","
        << "\"cpu_model\":" << json::quote(cpu_model) << ","
        << "\"logical_cpus\":" << logical_cpus << ","
        << "\"physical_cpus\":" << physical_cpus << ","
        << "\"performance_cores\":" << performance_cores << ","
        << "\"efficiency_cores\":" << efficiency_cores << ","
        << "\"cache_line_bytes\":" << cache_line_bytes << ","
        << "\"page_size_bytes\":" << page_size_bytes << ","
        << "\"total_memory_bytes\":" << total_memory_bytes << ","
        << "\"numa_node_count\":" << nodes.size() << ","
        << "\"is_multi_numa\":" << (is_multi_numa ? "true" : "false") << ","
        << "\"supports_hard_cpu_pinning\":" << (supports_hard_cpu_pinning ? "true" : "false") << ","
        << "\"supports_numa_memory_policy\":" << (supports_numa_memory_policy ? "true" : "false") << ","
        << "\"supports_hw_cache_counters\":" << (supports_hw_cache_counters ? "true" : "false") << ","
        << "\"has_libnuma\":" << (has_libnuma ? "true" : "false") << ","
        << "\"capability_notes\":" << json::quote(capability_notes) << ","
        << "\"nodes\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& n = nodes[i];
        oss << "{"
            << "\"node_id\":" << n.node_id << ","
            << "\"memory_bytes\":" << n.memory_bytes << ","
            << "\"cpu_ids\":[";
        for (std::size_t j = 0; j < n.cpu_ids.size(); ++j) {
            if (j > 0) oss << ",";
            oss << n.cpu_ids[j];
        }
        oss << "],\"distances\":[";
        for (std::size_t j = 0; j < n.distances.size(); ++j) {
            if (j > 0) oss << ",";
            oss << n.distances[j];
        }
        oss << "]}";
    }
    oss << "]}";
    return oss.str();
}

TopologyInfo detect_topology() {
    TopologyInfo info;
    info.has_libnuma = (NUMA_ENGINE_HAS_LIBNUMA != 0);

    const unsigned hw = std::thread::hardware_concurrency();
    info.logical_cpus = hw > 0 ? static_cast<std::size_t>(hw) : 1;
    info.physical_cpus = info.logical_cpus;

#if defined(__APPLE__)
    struct utsname uts {};
    if (uname(&uts) == 0) {
        info.os_name = uts.sysname;
        info.os_release = uts.release;
        info.arch = uts.machine;
    } else {
        info.os_name = "Darwin";
        info.arch = "arm64";
    }

    info.cpu_model = read_sysctl_string("machdep.cpu.brand_string");
    if (info.cpu_model.empty()) {
        info.cpu_model = read_sysctl_string("hw.model");
    }

    std::int32_t v32 = 0;
    std::int64_t v64 = 0;
    if (read_sysctl("hw.ncpu", v32) && v32 > 0) info.logical_cpus = static_cast<std::size_t>(v32);
    if (read_sysctl("hw.physicalcpu", v32) && v32 > 0) info.physical_cpus = static_cast<std::size_t>(v32);
    if (read_sysctl("hw.perflevel0.physicalcpu", v32) && v32 > 0) info.performance_cores = static_cast<std::size_t>(v32);
    if (read_sysctl("hw.perflevel1.physicalcpu", v32) && v32 > 0) info.efficiency_cores = static_cast<std::size_t>(v32);
    if (read_sysctl("hw.cachelinesize", v64) && v64 > 0) info.cache_line_bytes = static_cast<std::size_t>(v64);
    if (read_sysctl("hw.pagesize", v64) && v64 > 0) info.page_size_bytes = static_cast<std::size_t>(v64);
    if (read_sysctl("hw.memsize", v64) && v64 > 0) info.total_memory_bytes = static_cast<std::uint64_t>(v64);

    NumaNodeInfo node0;
    node0.node_id = 0;
    node0.memory_bytes = info.total_memory_bytes;
    node0.distances = {10};
    for (std::size_t c = 0; c < info.logical_cpus; ++c) {
        node0.cpu_ids.push_back(static_cast<int>(c));
    }
    info.nodes.push_back(std::move(node0));

    info.is_multi_numa = false;
    info.supports_hard_cpu_pinning = false;
    info.supports_numa_memory_policy = false;
    info.supports_hw_cache_counters = false;
    info.capability_notes =
        "Single-node UMA host (macOS). Cross-node NUMA placement (remote/interleaved), "
        "Linux hard CPU pinning (sched_setaffinity), and perf_event_open hardware cache "
        "counters are unavailable on this platform and are labeled explicitly.";

#elif defined(__linux__)
    struct utsname uts {};
    if (uname(&uts) == 0) {
        info.os_name = uts.sysname;
        info.os_release = uts.release;
        info.arch = uts.machine;
    } else {
        info.os_name = "Linux";
    }

    const long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) info.page_size_bytes = static_cast<std::size_t>(ps);
#if defined(_SC_LEVEL1_DCACHE_LINESIZE)
    const long cls = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (cls > 0) info.cache_line_bytes = static_cast<std::size_t>(cls);
#endif
    const long pages = sysconf(_SC_PHYS_PAGES);
    if (pages > 0 && ps > 0) {
        info.total_memory_bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(ps);
    }

    // Read CPU model from /proc/cpuinfo
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    info.cpu_model = trim(line.substr(colon + 1));
                    break;
                }
            }
        }
        if (info.cpu_model.empty()) {
            info.cpu_model = info.arch + " Linux CPU";
        }
    }

    // Discover NUMA nodes under /sys/devices/system/node
    const std::filesystem::path node_root("/sys/devices/system/node");
    if (std::filesystem::exists(node_root)) {
        std::vector<int> node_ids;
        for (const auto& entry : std::filesystem::directory_iterator(node_root)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("node", 0) == 0 && name.size() > 4) {
                const std::string suffix = name.substr(4);
                if (std::all_of(suffix.begin(), suffix.end(), ::isdigit)) {
                    node_ids.push_back(std::stoi(suffix));
                }
            }
        }
        std::sort(node_ids.begin(), node_ids.end());

        for (int nid : node_ids) {
            const auto node_dir = node_root / ("node" + std::to_string(nid));
            NumaNodeInfo ninfo;
            ninfo.node_id = nid;
            ninfo.cpu_ids = parse_linux_cpulist(read_first_line(node_dir / "cpulist"));
            ninfo.memory_bytes = parse_linux_meminfo_bytes(node_dir / "meminfo");
            const std::string dist_line = read_first_line(node_dir / "distance");
            std::istringstream dss(dist_line);
            int d = 0;
            while (dss >> d) {
                ninfo.distances.push_back(d);
            }
            if (!ninfo.cpu_ids.empty() || ninfo.memory_bytes > 0) {
                info.nodes.push_back(std::move(ninfo));
            }
        }
    }

    if (info.nodes.empty()) {
        NumaNodeInfo node0;
        node0.node_id = 0;
        node0.memory_bytes = info.total_memory_bytes;
        node0.distances = {10};
        for (std::size_t c = 0; c < info.logical_cpus; ++c) {
            node0.cpu_ids.push_back(static_cast<int>(c));
        }
        info.nodes.push_back(std::move(node0));
    }

    info.is_multi_numa = (info.nodes.size() >= 2);
    info.supports_hard_cpu_pinning = true;
    info.supports_numa_memory_policy = true;
    info.supports_hw_cache_counters = true;
    info.capability_notes = info.is_multi_numa
        ? "Multi-NUMA Linux host detected; hard CPU pinning and local/remote/interleaved NUMA policies supported."
        : "Single-node Linux host detected; hard CPU pinning supported, but cross-node NUMA scenarios (remote/interleaved) require >= 2 NUMA nodes.";
#else
    info.os_name = "Unknown";
    info.arch = "Unknown";
    info.cpu_model = "Unknown";
    NumaNodeInfo node0;
    node0.node_id = 0;
    node0.distances = {10};
    for (std::size_t c = 0; c < info.logical_cpus; ++c) {
        node0.cpu_ids.push_back(static_cast<int>(c));
    }
    info.nodes.push_back(std::move(node0));
    info.capability_notes = "Generic fallback topology.";
#endif

    return info;
}

TopologyInfo make_synthetic_topology(
    std::size_t num_nodes,
    std::size_t cpus_per_node,
    bool supports_hard_pinning,
    bool supports_numa_policy) {
    TopologyInfo info;
    info.os_name = supports_numa_policy ? "Linux-Synthetic" : "SingleNode-Synthetic";
    info.os_release = "synthetic-1.0";
    info.arch = "x86_64";
    info.cpu_model = "Synthetic Test CPU";
    info.logical_cpus = num_nodes * cpus_per_node;
    info.physical_cpus = info.logical_cpus;
    info.cache_line_bytes = 64;
    info.page_size_bytes = 4096;
    info.total_memory_bytes = static_cast<std::uint64_t>(num_nodes) * 16ULL * 1024ULL * 1024ULL * 1024ULL;
    info.is_multi_numa = (num_nodes >= 2);
    info.supports_hard_cpu_pinning = supports_hard_pinning;
    info.supports_numa_memory_policy = supports_numa_policy;
    info.supports_hw_cache_counters = supports_numa_policy;
    info.has_libnuma = supports_numa_policy;
    info.capability_notes = info.is_multi_numa
        ? "Synthetic multi-NUMA topology for unit testing."
        : "Synthetic single-node topology for fallback unit testing.";

    int next_cpu = 0;
    for (std::size_t n = 0; n < num_nodes; ++n) {
        NumaNodeInfo node;
        node.node_id = static_cast<int>(n);
        node.memory_bytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        for (std::size_t c = 0; c < cpus_per_node; ++c) {
            node.cpu_ids.push_back(next_cpu++);
        }
        for (std::size_t d = 0; d < num_nodes; ++d) {
            node.distances.push_back(d == n ? 10 : 21);
        }
        info.nodes.push_back(std::move(node));
    }
    return info;
}

}  // namespace numa_engine
