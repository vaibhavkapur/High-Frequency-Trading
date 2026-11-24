#pragma once

#include <cstdint>
#include <vector>

#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/types.hpp"

namespace xarb {

struct SynthFeedConfig {
  uint64_t seed{20261124ULL};
  uint32_t instrument_id{0};
  size_t num_steps{4000};                // Number of simulation ticks (~3 venue updates per step)
  TimestampNs start_ts_ns{1'730'000'000LL * kNsPerSec};
  TimestampNs step_mean_ns{5LL * kNsPerMs};  // ~5 ms between market updates (~200 Hz per venue)
  PriceTicks initial_mid_ticks{6'500'000};   // $65,000.00
  size_t book_levels{10};
  // Dislocation & microstructure mix:
  double fleeting_dislocation_prob{0.025};   // Short-lived (2-4 steps = 10-20 ms) cross-venue arb
  double persistent_dislocation_prob{0.012}; // Longer-lived (10-20 steps = 50-100 ms) cross-venue arb
  double fee_trapped_cross_prob{0.030};      // Gross positive (~12 bps), negative after ~20 bps taker fees
  double thin_depth_prob{0.25};              // Thin top level on dislocations
  // Fault injection (for testing recovery, staleness, and reconnect metrics):
  bool inject_faults{true};
};

// Generates a realistic, deterministic 3-venue recorded feed using authentic Binance, Coinbase,
// and OKX wire payloads (including valid OKX IEEE CRC32 checksums and venue-specific recovery sequences).
std::vector<RecordedFeedEvent> generate_synthetic_feed(const InstrumentRegistry& registry,
                                                       const SynthFeedConfig& cfg);

}  // namespace xarb
