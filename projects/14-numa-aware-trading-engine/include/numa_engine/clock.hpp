#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

namespace numa_engine {

// Monotonic high-resolution clock used uniformly for latency measurements.
// Uses CLOCK_MONOTONIC_RAW on Linux/macOS when available to avoid NTP slewing
// discontinuities during microsecond/nanosecond latency sampling.
class MonotonicClock {
public:
    [[nodiscard]] static inline std::uint64_t now_ns() noexcept {
#if defined(CLOCK_MONOTONIC_RAW)
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
#else
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
    }

    [[nodiscard]] static constexpr std::string_view clock_source_name() noexcept {
#if defined(CLOCK_MONOTONIC_RAW)
        return "CLOCK_MONOTONIC_RAW";
#else
        return "std::chrono::steady_clock";
#endif
    }

    // Lightweight CPU relax hint inside spin loops to reduce pipeline stalls
    // and interconnect traffic during busy-wait backpressure.
    static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm64__)
        asm volatile("yield" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }
};

}  // namespace numa_engine
