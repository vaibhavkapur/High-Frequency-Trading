#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"

namespace hft::runtime {

// Canonical L3/L2 observed order book for a single (VenueId, InstrumentId).
// Tracks resting feed orders by FeedOrderRef, enforces staleness & FeedHealth gating,
// and computes a deterministic FNV-1a state hash for differential / golden checks.
class CanonicalOrderBook {
 public:
  explicit CanonicalOrderBook(std::int64_t stale_threshold_ns = 50'000'000);

  void apply(const contracts::v1::MarketDataEvent& md, contracts::v1::SimTimeNs now_ns);
  void set_health(contracts::v1::FeedHealth health) noexcept { health_ = health; }
  void clear() noexcept;

  [[nodiscard]] contracts::v1::TopOfBook top_of_book(
      contracts::v1::SimTimeNs now_ns) const noexcept;
  [[nodiscard]] std::vector<contracts::v1::LevelView> depth(contracts::v1::Side side,
                                                            std::size_t max_levels) const;
  [[nodiscard]] bool is_stale(contracts::v1::SimTimeNs now_ns) const noexcept;
  [[nodiscard]] bool is_tradable(contracts::v1::SimTimeNs now_ns) const noexcept;
  [[nodiscard]] contracts::v1::FeedHealth health() const noexcept { return health_; }
  [[nodiscard]] contracts::v1::SimTimeNs last_update_ns() const noexcept { return last_update_ns_; }
  [[nodiscard]] std::uint64_t state_hash() const noexcept;
  [[nodiscard]] std::size_t live_order_count() const noexcept { return orders_.size(); }

 private:
  struct RestingFeedOrder {
    contracts::v1::Side side = contracts::v1::Side::Buy;
    contracts::v1::Price price{0};
    contracts::v1::Quantity qty{0};
  };

  struct LevelAgg {
    std::uint64_t total_qty = 0;
    std::uint32_t order_count = 0;
  };

  void add_to_level(contracts::v1::Side side, contracts::v1::Price px, std::uint64_t qty);
  void remove_from_level(contracts::v1::Side side, contracts::v1::Price px, std::uint64_t qty,
                         bool decrement_count);

  std::int64_t stale_threshold_ns_;
  contracts::v1::FeedHealth health_ = contracts::v1::FeedHealth::Healthy;
  contracts::v1::SimTimeNs last_update_ns_{0};
  bool has_received_any_ = false;

  std::unordered_map<std::uint64_t, RestingFeedOrder> orders_;
  std::map<std::int64_t, LevelAgg, std::greater<std::int64_t>> bids_;
  std::map<std::int64_t, LevelAgg, std::less<std::int64_t>> asks_;
};

}  // namespace hft::runtime
