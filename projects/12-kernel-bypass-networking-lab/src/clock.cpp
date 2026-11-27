#include "kbnl/clock.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace kbnl {

namespace {

#if defined(CLOCK_MONOTONIC_RAW)
constexpr clockid_t kClockId = CLOCK_MONOTONIC_RAW;
constexpr const char* kClockName = "CLOCK_MONOTONIC_RAW";
#else
constexpr clockid_t kClockId = CLOCK_MONOTONIC;
constexpr const char* kClockName = "CLOCK_MONOTONIC";
#endif

} // namespace

uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(kClockId, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

std::string utc_timestamp_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    ::gmtime_r(&t, &utc_tm);
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

uint64_t measure_clock_overhead_ns(size_t iterations) noexcept {
    if (iterations == 0) {
        return 0;
    }
    const uint64_t t0 = now_ns();
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < iterations; ++i) {
        sink = now_ns();
    }
    (void)sink;
    const uint64_t t1 = now_ns();
    if (t1 <= t0) {
        return 0;
    }
    return (t1 - t0) / iterations;
}

ClockMetadata get_clock_metadata() {
    ClockMetadata meta{};
    meta.name = kClockName;
    meta.api = "POSIX clock_gettime";
    timespec res{};
    if (::clock_getres(kClockId, &res) == 0) {
        meta.nominal_resolution_ns =
            static_cast<uint64_t>(res.tv_sec) * 1000000000ULL + static_cast<uint64_t>(res.tv_nsec);
    } else {
        meta.nominal_resolution_ns = 1;
    }
    meta.measured_call_overhead_ns = measure_clock_overhead_ns(20000);
    meta.is_monotonic = true;
    meta.same_host_domain_required = true;
#if defined(__APPLE__)
    meta.notes = "macOS Darwin monotonic raw clock (mach_continuous/mach_absolute backed); valid for same-host one-way latency only. Cross-host requires PTP/hardware timestamps.";
#elif defined(__linux__)
    meta.notes = "Linux CLOCK_MONOTONIC_RAW (vDSO TSC/cntvct backed, unaffected by NTP slew); valid for same-host latency. Cross-host requires PTP/SO_TIMESTAMPING.";
#else
    meta.notes = "POSIX monotonic clock; valid for same-host latency deltas only.";
#endif
    return meta;
}

} // namespace kbnl
