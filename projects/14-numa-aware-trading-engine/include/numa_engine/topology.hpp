#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace numa_engine {

struct NumaNodeInfo {
    int node_id = 0;
    std::vector<int> cpu_ids;
    std::uint64_t memory_bytes = 0;
    std::vector<int> distances;  // Relative NUMA distance to each node (10 = local on Linux)
};

struct TopologyInfo {
    std::string os_name;
    std::string os_release;
    std::string arch;
    std::string cpu_model;
    std::size_t logical_cpus = 1;
    std::size_t physical_cpus = 1;
    std::size_t performance_cores = 0;  // Non-zero on hybrid architectures (e.g., Apple Silicon)
    std::size_t efficiency_cores = 0;
    std::size_t cache_line_bytes = 64;
    std::size_t page_size_bytes = 4096;
    std::uint64_t total_memory_bytes = 0;
    std::vector<NumaNodeInfo> nodes;

    // Explicit platform capability flags so single-node / non-Linux environments
    // never silently claim hard CPU pinning or cross-node NUMA binding.
    bool is_multi_numa = false;
    bool supports_hard_cpu_pinning = false;
    bool supports_numa_memory_policy = false;
    bool supports_hw_cache_counters = false;
    bool has_libnuma = false;
    std::string capability_notes;

    [[nodiscard]] std::size_t numa_node_count() const noexcept { return nodes.size(); }
    [[nodiscard]] bool has_cpu(int cpu_id) const noexcept;
    [[nodiscard]] bool has_node(int node_id) const noexcept;
    [[nodiscard]] int node_for_cpu(int cpu_id) const noexcept;
    [[nodiscard]] std::string to_json() const;
};

// Inspects the live host machine topology.
[[nodiscard]] TopologyInfo detect_topology();

// Builds synthetic topologies for deterministic unit tests (e.g., simulating
// a 2-node multi-NUMA server or a 1-node workstation).
[[nodiscard]] TopologyInfo make_synthetic_topology(
    std::size_t num_nodes,
    std::size_t cpus_per_node,
    bool supports_hard_pinning,
    bool supports_numa_policy);

// Parses a Linux cpulist string like "0-3,8,10-11" into sorted CPU IDs.
[[nodiscard]] std::vector<int> parse_linux_cpulist(const std::string& cpulist);

}  // namespace numa_engine
