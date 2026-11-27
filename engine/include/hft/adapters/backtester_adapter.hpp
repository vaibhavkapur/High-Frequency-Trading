#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hft/contracts/v1/events.hpp"
#include "hftbt/feed.hpp"
#include "hftbt/latency.hpp"
#include "hftbt/synth.hpp"

namespace hft::adapters {

// Wraps Project 04's hftbt::LatencyPath to provide deterministic, FIFO-monotonic
// causal latency paths for MD delivery, order ingress, and response egress.
class CausalLatencyAdapter {
 public:
  CausalLatencyAdapter(std::int64_t md_base_ns, std::int64_t md_jitter_ns,
                       std::int64_t order_base_ns, std::int64_t order_jitter_ns,
                       std::int64_t resp_base_ns, std::int64_t resp_jitter_ns,
                       std::uint64_t seed = 42);

  [[nodiscard]] contracts::v1::SimTimeNs deliver_md(contracts::v1::SimTimeNs source_ns) noexcept;
  [[nodiscard]] contracts::v1::SimTimeNs deliver_order(contracts::v1::SimTimeNs sent_ns) noexcept;
  [[nodiscard]] contracts::v1::SimTimeNs deliver_response(
      contracts::v1::SimTimeNs exch_ns) noexcept;

 private:
  hftbt::LatencyPath md_path_;
  hftbt::LatencyPath order_path_;
  hftbt::LatencyPath resp_path_;
};

// Converts Project 04 synthetic/file L3 feeds into canonical Envelope(MarketDataEvent) streams.
class BacktesterFeedAdapter {
 public:
  static std::vector<contracts::v1::Envelope> generate_l3_envelopes(
      contracts::v1::VenueId venue, std::size_t event_count, std::uint32_t instruments,
      std::uint64_t seed, std::int64_t start_price_ticks = 10'000);

  static std::vector<contracts::v1::Envelope> from_hftbt_feed(
      contracts::v1::VenueId venue, const hftbt::FeedData& feed);
};

}  // namespace hft::adapters
