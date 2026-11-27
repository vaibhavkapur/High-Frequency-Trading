#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kbnl {

struct LatencySummary {
    uint64_t count = 0;
    uint64_t warmup_ignored = 0;
    uint64_t negative_clamped = 0;
    uint64_t min_ns = 0;
    uint64_t max_ns = 0;
    double mean_ns = 0.0;
    double stddev_ns = 0.0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    bool exact_quantiles = true;
};

// Records per-packet latency samples in nanoseconds with bounded memory.
// Keeps exact samples up to max_exact_samples (default 2,000,000) for exact quantiles,
// and falls back to deterministic logarithmic-linear bucketed histogram beyond that limit.
class LatencyStats {
public:
    explicit LatencyStats(size_t max_exact_samples = 2000000);

    void reset();
    void record_sample(uint64_t send_ts_ns, uint64_t recv_ts_ns, bool is_warmup) noexcept;
    void record_ns(uint64_t latency_ns, bool is_warmup = false) noexcept;

    LatencySummary summarize() const;

    uint64_t sample_count() const noexcept { return count_; }

private:
    static constexpr size_t kBucketCount = 256;
    static size_t bucket_index(uint64_t ns) noexcept;
    static uint64_t bucket_upper_bound_ns(size_t idx) noexcept;

    size_t max_exact_samples_;
    std::vector<uint64_t> samples_;
    std::vector<uint64_t> buckets_;

    uint64_t count_ = 0;
    uint64_t warmup_ignored_ = 0;
    uint64_t negative_clamped_ = 0;
    uint64_t min_ns_ = 0;
    uint64_t max_ns_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
};

} // namespace kbnl
