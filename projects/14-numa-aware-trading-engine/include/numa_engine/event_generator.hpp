#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "numa_engine/types.hpp"

namespace numa_engine {

struct GeneratorConfig {
    std::uint64_t seed = 42;
    std::size_t num_events = 100000;
    std::size_t warmup_events = 5000;
    std::uint16_t num_symbols = 16;
    std::int64_t base_price_ticks = 1'000'000;  // 100.0000 USD
};

// Validates a single MarketEvent against the v1.0.0 data contract.
[[nodiscard]] Error validate_event(
    const MarketEvent& ev,
    std::uint64_t previous_seq_id,
    std::size_t line_number = 0);

// Validates a full event stream (non-empty, strictly monotonic seq_id, valid fields).
[[nodiscard]] Error validate_event_stream(const std::vector<MarketEvent>& events);

// Validates generator parameters.
[[nodiscard]] Error validate_generator_config(const GeneratorConfig& cfg);

// Deterministic cross-platform synthetic market event generator using SplitMix64.
// Produces identical events across OS/architectures for the same GeneratorConfig.
[[nodiscard]] Result<std::vector<MarketEvent>> generate_synthetic_events(const GeneratorConfig& cfg);

// Loads and validates a CSV fixture matching the v1.0.0 schema:
// seq_id,symbol_id,event_type,side,price_ticks,quantity,order_id,flags
[[nodiscard]] Result<std::vector<MarketEvent>> load_events_csv(const std::string& path);

// Saves events to the canonical CSV format (for reproducible replay).
[[nodiscard]] Error save_events_csv(const std::string& path, const std::vector<MarketEvent>& events);

}  // namespace numa_engine
