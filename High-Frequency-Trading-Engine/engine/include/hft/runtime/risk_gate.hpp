#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"

namespace hft::runtime {

struct RiskConfig {
  std::uint64_t max_order_qty_lots = 200;
  contracts::v1::Money max_order_notional{500'000LL * contracts::v1::kMoneyScale};
  std::int64_t max_long_position_lots = 500;
  std::int64_t max_short_position_lots = 500;
  std::size_t max_open_orders = 64;
  std::uint32_t max_price_band_bps = 500;  // 5.00% collar around observed mid
  bool require_tradable_market = true;
};

struct ExposureSnapshot {
  std::int64_t confirmed_position = 0;
  std::uint64_t pending_buy_lots = 0;   // includes PendingNew, Live, Uncertain, PendingCancel
  std::uint64_t pending_sell_lots = 0;  // includes PendingNew, Live, Uncertain, PendingCancel
  std::size_t open_order_count = 0;
};

struct RiskDecision {
  bool approved = false;
  contracts::v1::RejectReason reason = contracts::v1::RejectReason::None;
  contracts::v1::ErrorCode error = contracts::v1::ErrorCode::Ok;
};

struct RiskStats {
  std::uint64_t checked = 0;
  std::uint64_t approved = 0;
  std::uint64_t rejected = 0;
  std::uint64_t kill_switch_rejects = 0;
  std::uint64_t stale_market_rejects = 0;
  std::uint64_t venue_down_rejects = 0;
  std::uint64_t position_limit_rejects = 0;
  std::uint64_t size_or_notional_rejects = 0;
  std::uint64_t duplicate_token_rejects = 0;
  std::uint64_t price_band_rejects = 0;
};

// Local pre-trade risk gate enforced before any order leaves for a gateway/venue.
// Accounts for worst-case exposure (confirmed position +/- ALL pending & uncertain open orders).
class PreTradeRiskGate {
 public:
  explicit PreTradeRiskGate(RiskConfig cfg,
                            std::vector<contracts::v1::InstrumentMeta> instruments);

  void set_kill_switch(bool active) noexcept { kill_switch_ = active; }
  [[nodiscard]] bool kill_switch_active() const noexcept { return kill_switch_; }

  void set_venue_healthy(contracts::v1::VenueId venue, bool healthy);
  [[nodiscard]] bool is_venue_healthy(contracts::v1::VenueId venue) const noexcept;

  RiskDecision check_order(contracts::v1::VenueId venue, contracts::v1::InstrumentId instrument,
                           const contracts::v1::OrderIntent& intent,
                           const contracts::v1::TopOfBook& observed_tob, bool market_tradable,
                           const ExposureSnapshot& exposure);

  [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const RiskConfig& config() const noexcept { return cfg_; }

 private:
  RiskConfig cfg_;
  std::vector<contracts::v1::InstrumentMeta> instruments_;
  bool kill_switch_ = false;
  std::unordered_map<contracts::v1::VenueId, bool> venue_healthy_;
  std::unordered_set<contracts::v1::ClientToken, contracts::v1::ClientTokenHash> seen_tokens_;
  RiskStats stats_{};
};

}  // namespace hft::runtime
