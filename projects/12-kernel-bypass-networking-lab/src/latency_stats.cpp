#include "kbnl/latency_stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kbnl {

LatencyStats::LatencyStats(size_t max_exact_samples)
    : max_exact_samples_(max_exact_samples),
      buckets_(kBucketCount, 0) {
    samples_.reserve(std::min<size_t>(max_exact_samples_, 65536));
}

void LatencyStats::reset() {
    samples_.clear();
    std::fill(buckets_.begin(), buckets_.end(), 0);
    count_ = 0;
    warmup_ignored_ = 0;
    negative_clamped_ = 0;
    min_ns_ = 0;
    max_ns_ = 0;
    mean_ = 0.0;
    m2_ = 0.0;
}

size_t LatencyStats::bucket_index(uint64_t ns) noexcept {
    // Buckets 0..127: 100ns linear steps up to 12.8us
    // Buckets 128..191: 1us linear steps up to 76.8us
    // Buckets 192..223: 10us linear steps up to 396.8us
    // Buckets 224..247: 100us linear steps up to 2.7968ms
    // Buckets 248..255: 1ms+ overflow buckets
    if (ns < 12800ULL) {
        return static_cast<size_t>(ns / 100ULL);
    }
    if (ns < 76800ULL) {
        return 128 + static_cast<size_t>((ns - 12800ULL) / 1000ULL);
    }
    if (ns < 396800ULL) {
        return 192 + static_cast<size_t>((ns - 76800ULL) / 10000ULL);
    }
    if (ns < 2796800ULL) {
        return 224 + static_cast<size_t>((ns - 396800ULL) / 100000ULL);
    }
    const uint64_t ms_over = (ns - 2796800ULL) / 1000000ULL;
    const size_t idx = 248 + static_cast<size_t>(std::min<uint64_t>(ms_over, 7));
    return idx;
}

uint64_t LatencyStats::bucket_upper_bound_ns(size_t idx) noexcept {
    if (idx < 128) return (idx + 1) * 100ULL;
    if (idx < 192) return 12800ULL + (idx - 128 + 1) * 1000ULL;
    if (idx < 224) return 76800ULL + (idx - 192 + 1) * 10000ULL;
    if (idx < 248) return 396800ULL + (idx - 224 + 1) * 100000ULL;
    return 2796800ULL + (idx - 248 + 1) * 1000000ULL;
}

void LatencyStats::record_sample(uint64_t send_ts_ns, uint64_t recv_ts_ns, bool is_warmup) noexcept {
    if (is_warmup) {
        ++warmup_ignored_;
        return;
    }
    uint64_t delta = 0;
    if (recv_ts_ns >= send_ts_ns) {
        delta = recv_ts_ns - send_ts_ns;
    } else {
        ++negative_clamped_;
        delta = 0;
    }
    record_ns(delta, false);
}

void LatencyStats::record_ns(uint64_t latency_ns, bool is_warmup) noexcept {
    if (is_warmup) {
        ++warmup_ignored_;
        return;
    }
    ++count_;
    if (count_ == 1) {
        min_ns_ = latency_ns;
        max_ns_ = latency_ns;
    } else {
        min_ns_ = std::min(min_ns_, latency_ns);
        max_ns_ = std::max(max_ns_, latency_ns);
    }

    const double x = static_cast<double>(latency_ns);
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(count_);
    const double delta2 = x - mean_;
    m2_ += delta * delta2;

    ++buckets_[bucket_index(latency_ns)];

    if (samples_.size() < max_exact_samples_) {
        samples_.push_back(latency_ns);
    }
}

LatencySummary LatencyStats::summarize() const {
    LatencySummary s{};
    s.count = count_;
    s.warmup_ignored = warmup_ignored_;
    s.negative_clamped = negative_clamped_;
    if (count_ == 0) {
        return s;
    }
    s.min_ns = min_ns_;
    s.max_ns = max_ns_;
    s.mean_ns = mean_;
    s.stddev_ns = (count_ > 1) ? std::sqrt(m2_ / static_cast<double>(count_ - 1)) : 0.0;

    if (count_ == samples_.size()) {
        s.exact_quantiles = true;
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) -> uint64_t {
            const double rank = p * static_cast<double>(sorted.size() - 1);
            const size_t idx = static_cast<size_t>(std::ceil(rank));
            return sorted[std::min(idx, sorted.size() - 1)];
        };
        s.p50_ns = pct(0.50);
        s.p95_ns = pct(0.95);
        s.p99_ns = pct(0.99);
        s.p999_ns = pct(0.999);
    } else {
        s.exact_quantiles = false;
        auto bucket_pct = [&](double p) -> uint64_t {
            const uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
            uint64_t acc = 0;
            for (size_t i = 0; i < kBucketCount; ++i) {
                acc += buckets_[i];
                if (acc >= target) {
                    return std::min(bucket_upper_bound_ns(i), max_ns_);
                }
            }
            return max_ns_;
        };
        s.p50_ns = bucket_pct(0.50);
        s.p95_ns = bucket_pct(0.95);
        s.p99_ns = bucket_pct(0.99);
        s.p999_ns = bucket_pct(0.999);
    }
    return s;
}

} // namespace kbnl
