#include "numa_engine/perf_counters.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <sys/resource.h>

#include "numa_engine/json_utils.hpp"

#if defined(__linux__)
#include <cerrno>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace numa_engine {

namespace {

std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    const double idx_f = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
    const std::size_t idx = std::min<std::size_t>(
        sorted.size() - 1,
        static_cast<std::size_t>(std::ceil(idx_f)));
    return sorted[idx];
}

void read_rusage_counters(
    std::uint64_t& minflt,
    std::uint64_t& majflt,
    std::uint64_t& nvcsw,
    std::uint64_t& nivcsw) {
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        minflt = static_cast<std::uint64_t>(ru.ru_minflt);
        majflt = static_cast<std::uint64_t>(ru.ru_majflt);
        nvcsw = static_cast<std::uint64_t>(ru.ru_nvcsw);
        nivcsw = static_cast<std::uint64_t>(ru.ru_nivcsw);
    }
}

#if defined(__linux__)
int open_perf_hw_counter(std::uint64_t config) {
    struct perf_event_attr pe {};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.inherit = 1;  // Include child producer/consumer threads
    return static_cast<int>(syscall(SYS_perf_event_open, &pe, 0, -1, -1, 0));
}

std::uint64_t read_perf_fd(int fd) {
    if (fd < 0) return 0;
    std::uint64_t val = 0;
    if (read(fd, &val, sizeof(val)) != static_cast<ssize_t>(sizeof(val))) {
        return 0;
    }
    return val;
}
#endif

}  // namespace

LatencyHistogram::LatencyHistogram(std::size_t reserve_samples) {
    if (reserve_samples > 0) {
        samples_.reserve(reserve_samples);
    }
}

void LatencyHistogram::reset() {
    samples_.clear();
    std::memset(bucket_counts_, 0, sizeof(bucket_counts_));
}

void LatencyHistogram::record(std::uint64_t latency_ns) {
    samples_.push_back(latency_ns);
    std::size_t bucket = 0;
    if (latency_ns > 0) {
        std::uint64_t v = latency_ns;
        while (v > 1 && bucket + 1 < kNumBuckets) {
            v >>= 1;
            ++bucket;
        }
    }
    ++bucket_counts_[bucket];
}

LatencySummary LatencyHistogram::summarize() const {
    LatencySummary s{};
    if (samples_.empty()) return s;

    std::vector<std::uint64_t> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());

    s.sample_count = sorted.size();
    s.min_ns = sorted.front();
    s.p50_ns = percentile_sorted(sorted, 50.0);
    s.p90_ns = percentile_sorted(sorted, 90.0);
    s.p99_ns = percentile_sorted(sorted, 99.0);
    s.p99_9_ns = percentile_sorted(sorted, 99.9);
    s.p99_99_ns = percentile_sorted(sorted, 99.99);
    s.max_ns = sorted.back();

    long double sum = 0.0L;
    for (std::uint64_t v : sorted) {
        sum += static_cast<long double>(v);
    }
    const long double mean = sum / static_cast<long double>(sorted.size());
    s.mean_ns = static_cast<double>(mean);

    long double sq_diff = 0.0L;
    for (std::uint64_t v : sorted) {
        const long double d = static_cast<long double>(v) - mean;
        sq_diff += d * d;
    }
    s.stddev_ns = static_cast<double>(std::sqrt(sq_diff / static_cast<long double>(sorted.size())));
    return s;
}

std::vector<HistogramBucket> LatencyHistogram::buckets() const {
    std::vector<HistogramBucket> out;
    out.reserve(kNumBuckets);
    for (std::size_t i = 0; i < kNumBuckets; ++i) {
        if (bucket_counts_[i] == 0) continue;
        const std::uint64_t lo = (i == 0) ? 0ULL : (1ULL << (i - 1));
        const std::uint64_t hi = (1ULL << i);
        out.push_back(HistogramBucket{lo, hi, bucket_counts_[i]});
    }
    return out;
}

std::string LatencySummary::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"sample_count\":" << sample_count << ","
        << "\"min_ns\":" << min_ns << ","
        << "\"p50_ns\":" << p50_ns << ","
        << "\"p90_ns\":" << p90_ns << ","
        << "\"p99_ns\":" << p99_ns << ","
        << "\"p99_9_ns\":" << p99_9_ns << ","
        << "\"p99_99_ns\":" << p99_99_ns << ","
        << "\"max_ns\":" << max_ns << ","
        << "\"mean_ns\":" << json::format_double(mean_ns, 2) << ","
        << "\"stddev_ns\":" << json::format_double(stddev_ns, 2)
        << "}";
    return oss.str();
}

std::string PerfCounterSnapshot::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"hw_counters_available\":" << (hw_counters_available ? "true" : "false") << ","
        << "\"hw_counters_note\":" << json::quote(hw_counters_note) << ","
        << "\"cache_references\":" << cache_references << ","
        << "\"cache_misses\":" << cache_misses << ","
        << "\"cache_miss_rate_pct\":" << json::format_double(cache_miss_rate_pct, 4) << ","
        << "\"cpu_cycles\":" << cpu_cycles << ","
        << "\"instructions\":" << instructions << ","
        << "\"minor_page_faults\":" << minor_page_faults << ","
        << "\"major_page_faults\":" << major_page_faults << ","
        << "\"voluntary_ctx_switches\":" << voluntary_ctx_switches << ","
        << "\"involuntary_ctx_switches\":" << involuntary_ctx_switches
        << "}";
    return oss.str();
}

PerfCounterCollector::PerfCounterCollector() {
#if defined(__linux__)
    fd_cache_refs_ = open_perf_hw_counter(PERF_COUNT_HW_CACHE_REFERENCES);
    fd_cache_misses_ = open_perf_hw_counter(PERF_COUNT_HW_CACHE_MISSES);
    fd_cycles_ = open_perf_hw_counter(PERF_COUNT_HW_CPU_CYCLES);
    fd_instructions_ = open_perf_hw_counter(PERF_COUNT_HW_INSTRUCTIONS);

    if (fd_cache_refs_ >= 0 && fd_cache_misses_ >= 0) {
        hw_available_ = true;
        hw_note_ = "Linux perf_event_open hardware cache counters active.";
    } else {
        const int err = errno;
        hw_available_ = false;
        hw_note_ = "Linux perf_event_open unavailable (errno=" + std::to_string(err) +
                   "; check /proc/sys/kernel/perf_event_paranoid or VM PMU virtualization). "
                   "Software getrusage counters still collected.";
        close_fds();
    }
#elif defined(__APPLE__)
    hw_available_ = false;
    hw_note_ = "Hardware PMU cache counters (perf_event_open) are unavailable on macOS; "
               "OS getrusage software counters (page faults, context switches) are collected.";
#else
    hw_available_ = false;
    hw_note_ = "Hardware PMU cache counters unsupported on this platform.";
#endif
}

PerfCounterCollector::~PerfCounterCollector() {
    close_fds();
}

void PerfCounterCollector::close_fds() noexcept {
#if defined(__linux__)
    auto close_one = [](int& fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };
    close_one(fd_cache_refs_);
    close_one(fd_cache_misses_);
    close_one(fd_cycles_);
    close_one(fd_instructions_);
#endif
}

void PerfCounterCollector::start() {
    read_rusage_counters(start_minflt_, start_majflt_, start_nvcsw_, start_nivcsw_);
#if defined(__linux__)
    if (hw_available_) {
        for (int fd : {fd_cache_refs_, fd_cache_misses_, fd_cycles_, fd_instructions_}) {
            if (fd >= 0) {
                ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }
    }
#endif
}

PerfCounterSnapshot PerfCounterCollector::stop() {
    PerfCounterSnapshot snap{};
    snap.hw_counters_available = hw_available_;
    snap.hw_counters_note = hw_note_;

#if defined(__linux__)
    if (hw_available_) {
        for (int fd : {fd_cache_refs_, fd_cache_misses_, fd_cycles_, fd_instructions_}) {
            if (fd >= 0) {
                ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }
        snap.cache_references = read_perf_fd(fd_cache_refs_);
        snap.cache_misses = read_perf_fd(fd_cache_misses_);
        snap.cpu_cycles = read_perf_fd(fd_cycles_);
        snap.instructions = read_perf_fd(fd_instructions_);
        if (snap.cache_references > 0) {
            snap.cache_miss_rate_pct =
                (static_cast<double>(snap.cache_misses) * 100.0) /
                static_cast<double>(snap.cache_references);
        }
    }
#endif

    std::uint64_t end_minflt = 0, end_majflt = 0, end_nvcsw = 0, end_nivcsw = 0;
    read_rusage_counters(end_minflt, end_majflt, end_nvcsw, end_nivcsw);
    snap.minor_page_faults = end_minflt >= start_minflt_ ? (end_minflt - start_minflt_) : 0;
    snap.major_page_faults = end_majflt >= start_majflt_ ? (end_majflt - start_majflt_) : 0;
    snap.voluntary_ctx_switches = end_nvcsw >= start_nvcsw_ ? (end_nvcsw - start_nvcsw_) : 0;
    snap.involuntary_ctx_switches = end_nivcsw >= start_nivcsw_ ? (end_nivcsw - start_nivcsw_) : 0;
    return snap;
}

}  // namespace numa_engine
