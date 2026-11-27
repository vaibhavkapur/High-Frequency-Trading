#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "numa_engine/affinity.hpp"
#include "numa_engine/matching_engine.hpp"
#include "numa_engine/memory.hpp"
#include "numa_engine/perf_counters.hpp"
#include "numa_engine/spsc_queue.hpp"
#include "numa_engine/topology.hpp"
#include "numa_engine/types.hpp"

namespace numa_engine {

struct PipelineConfig {
    std::string scenario_name = "default";
    PipelineKind pipeline_kind = PipelineKind::kLockFreeSpsc;
    MemoryPolicy memory_policy = MemoryPolicy::kDefault;
    std::size_t queue_capacity = 4096;

    // Thread placement (-1 means unpinned)
    int producer_cpu = -1;
    int consumer_cpu = -1;
    int preferred_memory_node = 0;

    // First-touch policy: when true with kFirstTouchLocal, consumer thread faults
    // queue memory after pinning.
    bool consumer_first_touch = true;

    // Strict vs fallback handling for single-node / non-Linux systems
    bool allow_single_node_fallback = true;
    bool require_hard_pinning = false;

    // Optional consumer synthetic work (busy-loop iterations per event) to test
    // controlled producer/consumer backpressure.
    std::uint32_t consumer_work_iters = 0;

    // Provenance metadata
    std::uint64_t seed = 42;
    std::size_t warmup_events = 0;
    std::string input_source = "synthetic";
};

struct PipelineRunResult {
    std::string schema_version = std::string(kSchemaVersion);
    std::string run_id;
    std::string scenario_name;
    PipelineKind pipeline_kind = PipelineKind::kLockFreeSpsc;
    MemoryPolicy requested_policy = MemoryPolicy::kDefault;
    MemoryPolicy effective_policy = MemoryPolicy::kDefault;
    ScenarioStatus status = ScenarioStatus::kExecuted;
    std::string status_note;

    std::size_t queue_capacity = 0;
    std::uint64_t seed = 0;
    std::string input_source;
    std::string clock_source;

    AffinityResult producer_affinity{};
    AffinityResult consumer_affinity{};
    AllocationReport allocation{};

    std::uint64_t events_sent = 0;
    std::uint64_t events_received = 0;
    std::uint64_t warmup_events = 0;
    std::uint64_t measured_events = 0;
    std::uint64_t lost_events = 0;

    double elapsed_ms = 0.0;
    double throughput_eps = 0.0;  // Measured events per second

    LatencySummary queue_latency{};   // send_ts -> recv_ts
    LatencySummary e2e_latency{};     // send_ts -> done_ts
    QueueBackpressureStats backpressure{};
    PerfCounterSnapshot perf_counters{};
    EngineSummary engine_summary{};

    [[nodiscard]] std::string to_json() const;
};

struct ComparisonRunConfig {
    std::size_t num_events = 200000;
    std::size_t warmup_events = 10000;
    std::size_t queue_capacity = 4096;
    std::uint16_t num_symbols = 16;
    std::uint64_t seed = 42;
    std::size_t repetitions = 5;
    bool allow_single_node_fallback = false;
    std::uint32_t consumer_work_iters = 0;
};

struct ScenarioAggregate {
    std::string scenario_name;
    PipelineKind pipeline_kind = PipelineKind::kLockFreeSpsc;
    MemoryPolicy requested_policy = MemoryPolicy::kDefault;
    ScenarioStatus status = ScenarioStatus::kExecuted;
    std::string status_note;
    std::size_t repetitions = 0;
    std::vector<PipelineRunResult> runs;

    double mean_throughput_eps = 0.0;
    double min_throughput_eps = 0.0;
    double max_throughput_eps = 0.0;
    double median_p50_ns = 0.0;
    double median_p99_ns = 0.0;
    double median_p99_9_ns = 0.0;
    std::uint64_t total_lost_events = 0;
    bool all_digests_match_baseline = false;

    [[nodiscard]] std::string to_json() const;
};

struct ComparisonReport {
    std::string schema_version = std::string(kSchemaVersion);
    std::string report_id;
    std::string build_type;
    std::string compiler;
    std::string clock_source;
    TopologyInfo topology;
    ComparisonRunConfig config;
    std::uint64_t baseline_digest = 0;
    bool functional_equivalence_passed = false;
    bool zero_lost_events = false;
    std::vector<ScenarioAggregate> scenarios;

    [[nodiscard]] std::string to_json() const;
};

// Validates a PipelineConfig against topology and contract rules.
[[nodiscard]] Error validate_pipeline_config(
    const PipelineConfig& cfg,
    const TopologyInfo& topology);

// Runs a single event pipeline execution over the supplied event stream.
// Note: pass an explicit TopologyInfo to test single-node or multi-node handling.
[[nodiscard]] Result<PipelineRunResult> run_pipeline(
    const PipelineConfig& cfg,
    const std::vector<MarketEvent>& events,
    const TopologyInfo& topology);

// Runs baseline + all placement scenarios (default_unpinned, local_pinned,
// first_touch_local, remote_node, interleaved) with repeated trials, verifying
// functional equivalence and labeling unavailable NUMA scenarios.
[[nodiscard]] Result<ComparisonReport> run_comparison_suite(
    const ComparisonRunConfig& cfg,
    const TopologyInfo& topology);

// Parses a minimal JSON config file into PipelineConfig (strict validation; no silent defaults for missing required fields).
[[nodiscard]] Result<PipelineConfig> load_pipeline_config_json(const std::string& path);

}  // namespace numa_engine
