#include "hft/runtime/risk_gate.hpp"

#include <cmath>

namespace hft::runtime {

using namespace contracts::v1;

PreTradeRiskGate::PreTradeRiskGate(RiskConfig cfg, std::vector<InstrumentMeta> instruments)
    : cfg_(cfg), instruments_(std::move(instruments)) {}

void PreTradeRiskGate::set_venue_healthy(VenueId venue, bool healthy) {
  venue_healthy_[venue] = healthy;
}

bool PreTradeRiskGate::is_venue_healthy(VenueId venue) const noexcept {
  const auto it = venue_healthy_.find(venue);
  return (it == venue_healthy_.end()) ? true : it->second;
}

RiskDecision PreTradeRiskGate::check_order(VenueId venue, InstrumentId instrument,
                                           const OrderIntent& intent,
                                           const TopOfBook& observed_tob, bool market_tradable,
                                           const ExposureSnapshot& exposure) {
  ++stats_.checked;

  auto reject = [&](RejectReason r, ErrorCode e) {
    ++stats_.rejected;
    return RiskDecision{false, r, e};
  };

  if (instrument >= instruments_.size()) {
    return reject(RejectReason::UnknownSymbol, ErrorCode::InvalidArgument);
  }

  // Cancels are always risk-reducing and allowed even during kill-switch / stale markets,
  // unless the venue itself is completely unreachable.
  if (intent.action == OrderAction::Cancel) {
    if (!is_venue_healthy(venue)) {
      ++stats_.venue_down_rejects;
      return reject(RejectReason::RiskVenueDown, ErrorCode::VenueUnavailable);
    }
    ++stats_.approved;
    return RiskDecision{true, RejectReason::None, ErrorCode::Ok};
  }

  if (kill_switch_) {
    ++stats_.kill_switch_rejects;
    return reject(RejectReason::RiskKillSwitch, ErrorCode::RiskRejected);
  }

  if (!is_venue_healthy(venue)) {
    ++stats_.venue_down_rejects;
    return reject(RejectReason::RiskVenueDown, ErrorCode::VenueUnavailable);
  }

  if (cfg_.require_tradable_market && !market_tradable) {
    ++stats_.stale_market_rejects;
    return reject(RejectReason::RiskStaleMarket, ErrorCode::StaleMarketData);
  }

  if (intent.token.empty() || seen_tokens_.contains(intent.token)) {
    ++stats_.duplicate_token_rejects;
    return reject(RejectReason::DuplicateToken, ErrorCode::DuplicateToken);
  }

  const auto& inst = instruments_[instrument];
  if (intent.qty.lots == 0 || intent.qty.lots > cfg_.max_order_qty_lots ||
      intent.qty.lots > inst.max_qty_lots) {
    ++stats_.size_or_notional_rejects;
    return reject(RejectReason::RiskMaxOrderQty, ErrorCode::RiskRejected);
  }

  Price check_price = intent.limit_price;
  if (intent.order_type == OrderType::Limit) {
    if (!check_price.valid() || check_price.ticks > inst.max_price_ticks) {
      return reject(RejectReason::InvalidPrice, ErrorCode::InvalidArgument);
    }
    if (observed_tob.valid_two_sided() && cfg_.max_price_band_bps > 0) {
      const __int128 mid2 = static_cast<__int128>(observed_tob.bid->price.ticks) +
                            static_cast<__int128>(observed_tob.ask->price.ticks);
      const __int128 px2 = static_cast<__int128>(check_price.ticks) * 2;
      const __int128 diff = (px2 >= mid2) ? (px2 - mid2) : (mid2 - px2);
      // diff / mid2 > max_price_band_bps / 10000  <=>  diff * 10000 > mid2 * max_price_band_bps
      if (diff * 10'000 > mid2 * static_cast<__int128>(cfg_.max_price_band_bps)) {
        ++stats_.price_band_rejects;
        return reject(RejectReason::PriceBand, ErrorCode::RiskRejected);
      }
    }
  } else {
    // Market order notional reference uses observed opposite side.
    if (intent.side == Side::Buy && observed_tob.ask.has_value()) {
      check_price = observed_tob.ask->price;
    } else if (intent.side == Side::Sell && observed_tob.bid.has_value()) {
      check_price = observed_tob.bid->price;
    } else {
      ++stats_.stale_market_rejects;
      return reject(RejectReason::RiskStaleMarket, ErrorCode::StaleMarketData);
    }
  }

  const auto notional = inst.notional_scaled(check_price, intent.qty);
  if (!notional.has_value() || notional->scaled > cfg_.max_order_notional.scaled) {
    ++stats_.size_or_notional_rejects;
    return reject(RejectReason::RiskMaxNotional, ErrorCode::RiskRejected);
  }

  if (exposure.open_order_count >= cfg_.max_open_orders) {
    return reject(RejectReason::RiskMaxOpenOrders, ErrorCode::RiskRejected);
  }

  // Worst-case position accounting (confirmed + ALL pending/live/uncertain orders on that side).
  const __int128 qty_i128 = static_cast<__int128>(intent.qty.lots);
  if (intent.side == Side::Buy) {
    const __int128 worst_long = static_cast<__int128>(exposure.confirmed_position) +
                                static_cast<__int128>(exposure.pending_buy_lots) + qty_i128;
    if (worst_long > static_cast<__int128>(cfg_.max_long_position_lots)) {
      ++stats_.position_limit_rejects;
      return reject(RejectReason::RiskMaxPosition, ErrorCode::RiskRejected);
    }
  } else {
    const __int128 worst_short = static_cast<__int128>(exposure.confirmed_position) -
                                 static_cast<__int128>(exposure.pending_sell_lots) - qty_i128;
    if (worst_short < -static_cast<__int128>(cfg_.max_short_position_lots)) {
      ++stats_.position_limit_rejects;
      return reject(RejectReason::RiskMaxPosition, ErrorCode::RiskRejected);
    }
  }

  seen_tokens_.insert(intent.token);
  ++stats_.approved;
  return RiskDecision{true, RejectReason::None, ErrorCode::Ok};
}

}  // namespace hft::runtime
