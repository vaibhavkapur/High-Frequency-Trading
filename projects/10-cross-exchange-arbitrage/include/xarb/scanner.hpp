#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/types.hpp"
#include "xarb/venue_adapter.hpp"

namespace xarb {

enum class SignalRejectionReason : uint8_t {
  Actionable = 0,
  VenueNotSynced = 1,
  StaleQuote = 2,
  CrossedBook = 3,
  NoGrossCross = 4,
  InsufficientEdgeAfterFees = 5,
  InsufficientDepth = 6,
  InsufficientFunding = 7,
  CooldownActive = 8,
  BelowMinProfit = 9,
};

inline std::string_view rejection_reason_name(SignalRejectionReason r) noexcept {
  switch (r) {
    case SignalRejectionReason::Actionable: return "actionable";
    case SignalRejectionReason::VenueNotSynced: return "venue_not_synced";
    case SignalRejectionReason::StaleQuote: return "stale_quote";
    case SignalRejectionReason::CrossedBook: return "crossed_book";
    case SignalRejectionReason::NoGrossCross: return "no_gross_cross";
    case SignalRejectionReason::InsufficientEdgeAfterFees: return "insufficient_edge_after_fees";
    case SignalRejectionReason::InsufficientDepth: return "insufficient_depth";
    case SignalRejectionReason::InsufficientFunding: return "insufficient_funding";
    case SignalRejectionReason::CooldownActive: return "cooldown_active";
    case SignalRejectionReason::BelowMinProfit: return "below_min_profit";
  }
  return "unknown";
}

struct ScannerConfig {
  double min_net_edge_bps{2.0};          // Minimum net VWAP edge after taker fees + slippage buffer
  double slippage_buffer_bps{1.0};       // Conservative safety margin in bps
  double min_net_profit_quote{0.25};     // Minimum expected net profit in quote currency (e.g. USDT)
  double max_trade_notional_quote{25000.0};
  QtyLots max_trade_lots{0};             // 0 = derived from max_trade_notional_quote
  TimestampNs cooldown_ns{50LL * kNsPerMs};  // Per-route cooldown after firing an actionable signal
};

struct RouteFundingLimits {
  double buy_venue_quote_available{0.0};
  double sell_venue_base_available{0.0};
};

struct ArbitrageSignal {
  uint64_t signal_id{0};
  TimestampNs detect_ns{0};
  uint32_t instrument_id{0};
  VenueId buy_venue{VenueId::Binance};
  VenueId sell_venue{VenueId::Coinbase};
  SignalRejectionReason status{SignalRejectionReason::NoGrossCross};

  PriceTicks buy_top_ticks{0};
  PriceTicks sell_top_ticks{0};
  double buy_vwap_ticks{0.0};
  double sell_vwap_ticks{0.0};
  PriceTicks buy_limit_ticks{0};
  PriceTicks sell_limit_ticks{0};

  QtyLots depth_executable_lots{0};  // Profitable quantity available on displayed books
  QtyLots funded_executable_lots{0}; // Capped by pre-funded balances & risk limits
  size_t buy_levels_used{0};
  size_t sell_levels_used{0};

  double gross_spread_bps{0.0};
  double net_edge_bps{0.0};
  double estimated_fees_quote{0.0};
  double estimated_net_profit_quote{0.0};

  TimestampNs buy_quote_age_ns{0};
  TimestampNs sell_quote_age_ns{0};

  bool is_actionable() const noexcept { return status == SignalRejectionReason::Actionable; }
};

struct ScannerStats {
  uint64_t scans_performed{0};
  uint64_t gross_crosses_seen{0};
  uint64_t actionable_signals{0};
  std::array<uint64_t, 10> rejections_by_reason{};
};

class OpportunityScanner {
 public:
  explicit OpportunityScanner(ScannerConfig cfg = {}) : cfg_(cfg) {}

  const ScannerConfig& config() const noexcept { return cfg_; }
  const ScannerStats& stats() const noexcept { return stats_; }

  // Evaluates all ordered pairs (buy_venue, sell_venue) for one instrument at timestamp now_ns.
  // Funding callback returns available quote at buy_venue and available base at sell_venue.
  template <typename FundingFn>
  std::vector<ArbitrageSignal> scan_instrument(TimestampNs now_ns, const InstrumentSpec& inst,
                                               const std::array<const VenueAdapter*, kVenueCount>& adapters,
                                               FundingFn&& get_funding) {
    stats_.scans_performed += 1;
    std::vector<ArbitrageSignal> results;

    for (size_t b = 0; b < kVenueCount; ++b) {
      for (size_t s = 0; s < kVenueCount; ++s) {
        if (b == s) continue;
        const VenueAdapter* buy_ad = adapters[b];
        const VenueAdapter* sell_ad = adapters[s];
        if (!buy_ad || !sell_ad) continue;

        auto sig = evaluate_pair(now_ns, inst, *buy_ad, *sell_ad, get_funding(static_cast<VenueId>(b), static_cast<VenueId>(s)));
        if (sig.has_value()) {
          results.push_back(*sig);
        }
      }
    }
    return results;
  }

  // Evaluates a single directed route (buy at buy_ad, sell at sell_ad).
  // Returns a signal record whenever there is either a gross top-of-book cross OR an actionable opportunity
  // (or a suppressed cross due to staleness/unsynced state if books still hold crossed quotes).
  std::optional<ArbitrageSignal> evaluate_pair(TimestampNs now_ns, const InstrumentSpec& inst,
                                               const VenueAdapter& buy_ad, const VenueAdapter& sell_ad,
                                               RouteFundingLimits funding);

  void reset_cooldowns() noexcept { last_fire_ns_.fill(0); }

 private:
  static size_t route_index(uint32_t inst_id, VenueId b, VenueId s) noexcept {
    return (static_cast<size_t>(inst_id % 16U) * kVenueCount + static_cast<size_t>(b)) * kVenueCount +
           static_cast<size_t>(s);
  }

  ScannerConfig cfg_;
  ScannerStats stats_{};
  uint64_t next_signal_id_{1};
  std::array<TimestampNs, 16 * kVenueCount * kVenueCount> last_fire_ns_{};
};

}  // namespace xarb
