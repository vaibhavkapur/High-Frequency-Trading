#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "obce/types.hpp"

namespace obce {

enum class WorkloadProfile : uint8_t {
    L3OrderFlow = 1,       // Realistic synthetic L3 Add/Modify/Cancel/Execute order book stream
    L2TopOfBook = 2,       // Synthetic L2 aggregated depth updates across levels 0..9
    VolatileBursty = 3,    // High-volatility bursts with wide price/qty swings and symbol switches
    ExtremeDeltas = 4      // Stress workload with boundary int64/uint64 swings & timestamp jumps
};

struct GeneratorConfig {
    uint64_t seed = 42;
    size_t event_count = 10000;
    uint32_t num_symbols = 8;
    uint64_t start_seq_id = 1;
    uint64_t start_timestamp_ns = 1700000000000000000ULL;
    uint16_t schema_version = kSchemaVersionV2;
    WorkloadProfile profile = WorkloadProfile::L3OrderFlow;
};

// Deterministic seeded generator (SplitMix64 + Xoshiro256**, platform-independent)
std::vector<MarketEvent> generate_synthetic_events(const GeneratorConfig& cfg);

const char* workload_profile_name(WorkloadProfile profile) noexcept;

}  // namespace obce
