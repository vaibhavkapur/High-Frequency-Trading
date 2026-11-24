// Seeded synthetic order flow. This is test/benchmark input, not market
// data: results on it say nothing about real markets.
#pragma once

#include "hftbt/feed.hpp"

namespace hftbt {

struct SynthConfig {
    std::uint64_t seed = 1;
    std::size_t events = 100'000;
    std::uint32_t instruments = 1;
    Price initial_mid = 10'000;
    Timestamp start_ts = 1'000'000'000;
    Timestamp mean_gap_ns = 50'000;
};

// Self-consistent L3 stream: adds never cross, executions hit the oldest
// order at the best price (price-time priority).
FeedData generate_l3(const SynthConfig& cfg);

// Collapse an L3 stream into L2 (Trade + Level events), discarding the
// order-level detail a market-by-price feed would not carry.
FeedData l3_to_l2(const FeedData& l3);

} // namespace hftbt
