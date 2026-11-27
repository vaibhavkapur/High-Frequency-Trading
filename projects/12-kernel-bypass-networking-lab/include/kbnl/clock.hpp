#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kbnl {

struct ClockMetadata {
    std::string name;
    std::string api;
    uint64_t nominal_resolution_ns = 0;
    uint64_t measured_call_overhead_ns = 0;
    bool is_monotonic = true;
    bool same_host_domain_required = true;
    std::string notes;
};

// Monotonic steady timestamp in nanoseconds for same-host latency measurement.
uint64_t now_ns() noexcept;

// Wall-clock UTC ISO-8601 string for report metadata only (never used for latency deltas).
std::string utc_timestamp_iso8601();

// Estimates median overhead of now_ns() in nanoseconds.
uint64_t measure_clock_overhead_ns(size_t iterations = 10000) noexcept;

// Returns documented clock metadata for this platform and build.
ClockMetadata get_clock_metadata();

} // namespace kbnl
