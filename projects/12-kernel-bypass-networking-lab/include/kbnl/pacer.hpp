#pragma once

#include "kbnl/config.hpp"

#include <cstdint>

namespace kbnl {

// Pure deterministic packet pacing schedule (no I/O, no wall clocks).
// Given a WorkloadConfig and start timestamp, computes the scheduled send timestamp
// (offset in nanoseconds from start) for packet index `idx` (0-based across warmup + measured).
class PacerSchedule {
public:
    explicit PacerSchedule(const WorkloadConfig& workload) noexcept;

    uint64_t scheduled_offset_ns(uint64_t packet_index) const noexcept;
    bool is_warmup(uint64_t packet_index) const noexcept;
    bool is_burst_packet(uint64_t packet_index) const noexcept;
    uint64_t total_data_packets() const noexcept;

private:
    WorkloadConfig workload_;
};

// Spins/sleeps until monotonic now_ns() reaches target_ns.
// Hybrid strategy: sleeps coarsely if > 200us remaining, then busy-waits for sub-microsecond accuracy.
void pace_until_ns(uint64_t target_ns) noexcept;

} // namespace kbnl
