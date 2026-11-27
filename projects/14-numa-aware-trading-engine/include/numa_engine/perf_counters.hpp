#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace numa_engine {

struct LatencySummary {
    std::size_t sample_count = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p90_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t p99_9_ns = 0;
    std::uint64_t p99_99_ns = 0;
    std::uint64_t max_ns = 0;
    double mean_ns = 0.0;
    double stddev_ns = 0.0;

    [[nodiscard]] std::string to_json() const;
};

struct HistogramBucket {
    std::uint64_t lower_bound_ns = 0;
    std::uint64_t upper_bound_ns = 0;
    std::uint64_t count = 0;
};

// Pre-allocated latency recorder collecting exact samples (up to reserve capacity)
// and logarithmic histogram buckets without hot-path allocations.
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::size_t reserve_samples = 0);

    void reset();
    void record(std::uint64_t latency_ns);

    [[nodiscard]] LatencySummary summarize() const;
    [[nodiscard]] std::vector<HistogramBucket> buckets() const;

private:
    std::vector<std::uint64_t> samples_;
    static constexpr std::size_t kNumBuckets = 32;  // 0..2^31 ns (~2.1s)
    std::uint64_t bucket_counts_[kNumBuckets] = {};
};

struct PerfCounterSnapshot {
    bool hw_counters_available = false;
    std::string hw_counters_note;

    // Hardware PMU counters (Linux perf_event_open when permitted)
    std::uint64_t cache_references = 0;
    std::uint64_t cache_misses = 0;
    double cache_miss_rate_pct = 0.0;
    std::uint64_t cpu_cycles = 0;
    std::uint64_t instructions = 0;

    // OS software counters via getrusage (available on Linux and macOS)
    std::uint64_t minor_page_faults = 0;
    std::uint64_t major_page_faults = 0;
    std::uint64_t voluntary_ctx_switches = 0;
    std::uint64_t involuntary_ctx_switches = 0;

    [[nodiscard]] std::string to_json() const;
};

// Scoped collector for Linux perf_event_open hardware cache counters plus
// portable getrusage software counters (page faults, context switches).
class PerfCounterCollector {
public:
    PerfCounterCollector();
    ~PerfCounterCollector();

    PerfCounterCollector(const PerfCounterCollector&) = delete;
    PerfCounterCollector& operator=(const PerfCounterCollector&) = delete;

    void start();
    [[nodiscard]] PerfCounterSnapshot stop();

private:
    void close_fds() noexcept;

#if defined(__linux__)
    int fd_cache_refs_ = -1;
    int fd_cache_misses_ = -1;
    int fd_cycles_ = -1;
    int fd_instructions_ = -1;
#endif
    bool hw_available_ = false;
    std::string hw_note_;

    std::uint64_t start_minflt_ = 0;
    std::uint64_t start_majflt_ = 0;
    std::uint64_t start_nvcsw_ = 0;
    std::uint64_t start_nivcsw_ = 0;
};

}  // namespace numa_engine
