#pragma once

#include "mmsim/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mmsim {

enum class MarketRegime : uint8_t {
    MeanReverting = 0, // Balanced liquidity, mild adverse selection
    ToxicTrend = 1,    // Informed order flow sweeps book prior to fundamental drift
    HighVolBurst = 2,  // Periodic volatility spikes and wider spreads
    ThinBook = 3,      // Shallow depth, wide spreads, higher queue turnover
};

struct SynthParams {
    uint64_t seed = 1001;
    MarketRegime regime = MarketRegime::MeanReverting;
    double duration_sec = 30.0;
    PriceTicks initial_mid_ticks = 10'000; // $100.00 at $0.01 tick
    int book_depth_levels = 5;
    double base_event_rate_hz = 400.0;     // Average market events per second
    double vol_ticks_per_sqrt_sec = 1.2;   // Fundamental mid diffusion
    double informed_flow_prob = 0.20;      // Probability a trade carries directional alpha
    double informed_jump_ticks = 1.5;      // Post-trade drift on informed sweeps
    double mean_reversion_kappa = 0.08;    // Pull back toward initial_mid_ticks
    Qty typical_level_qty = 400;
};

[[nodiscard]] std::string regime_to_string(MarketRegime r);
bool parse_regime(const std::string& s, MarketRegime& out);

// Generates a deterministic, realistic stream of L2 BookDelta and Trade events.
std::vector<MarketEvent> generate_synthetic_session(const SynthParams& params);

} // namespace mmsim
