#include "kbnl/pacer.hpp"
#include "kbnl/clock.hpp"

#include <chrono>
#include <thread>

namespace kbnl {

PacerSchedule::PacerSchedule(const WorkloadConfig& workload) noexcept
    : workload_(workload) {}

uint64_t PacerSchedule::scheduled_offset_ns(uint64_t packet_index) const noexcept {
    if (workload_.pacing_mode == PacingMode::kBurst) {
        const uint64_t bsize = workload_.burst_size > 0 ? workload_.burst_size : 1;
        const uint64_t burst_idx = packet_index / bsize;
        return burst_idx * workload_.burst_interval_us * 1000ULL;
    }

    // Constant rate: integer division rounding to nearest nanosecond.
    const uint64_t pps = workload_.target_pps > 0 ? workload_.target_pps : 1;
    // Use __uint128_t to avoid overflow for large packet counts.
    const __uint128_t numerator = static_cast<__uint128_t>(packet_index) * 1000000000ULL;
    return static_cast<uint64_t>(numerator / pps);
}

bool PacerSchedule::is_warmup(uint64_t packet_index) const noexcept {
    return packet_index < workload_.warmup_packets;
}

bool PacerSchedule::is_burst_packet(uint64_t packet_index) const noexcept {
    (void)packet_index;
    return workload_.pacing_mode == PacingMode::kBurst;
}

uint64_t PacerSchedule::total_data_packets() const noexcept {
    return workload_.warmup_packets + workload_.measured_packets;
}

void pace_until_ns(uint64_t target_ns) noexcept {
    while (true) {
        const uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        const uint64_t remaining = target_ns - now;
        if (remaining > 500000ULL) { // > 500 us: sleep conservatively
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 250000ULL));
        } else if (remaining > 5000ULL) {
            std::this_thread::yield();
        }
        // Final < 5 us: tight spin for deterministic release timing.
    }
}

} // namespace kbnl
