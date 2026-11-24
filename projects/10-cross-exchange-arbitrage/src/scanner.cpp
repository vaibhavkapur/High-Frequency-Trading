#include "xarb/scanner.hpp"

#include <algorithm>
#include <cmath>

namespace xarb {

std::optional<ArbitrageSignal> OpportunityScanner::evaluate_pair(TimestampNs now_ns, const InstrumentSpec& inst,
                                                                 const VenueAdapter& buy_ad,
                                                                 const VenueAdapter& sell_ad,
                                                                 RouteFundingLimits funding) {
  const auto b_ask = buy_ad.book().best_ask();
  const auto s_bid = sell_ad.book().best_bid();

  // Check staleness / sync state first.
  const bool buy_synced = (buy_ad.state() == BookState::Synced);
  const bool sell_synced = (sell_ad.state() == BookState::Synced);
  const bool buy_tradable = buy_ad.is_tradable(now_ns);
  const bool sell_tradable = sell_ad.is_tradable(now_ns);

  const bool has_gross_cross = b_ask.has_value() && s_bid.has_value() && s_bid->price > b_ask->price;

  if (!buy_synced || !sell_synced) {
    if (has_gross_cross) {
      stats_.gross_crosses_seen += 1;
      stats_.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::VenueNotSynced)] += 1;
      ArbitrageSignal sig;
      sig.signal_id = next_signal_id_++;
      sig.detect_ns = now_ns;
      sig.instrument_id = inst.id;
      sig.buy_venue = buy_ad.venue();
      sig.sell_venue = sell_ad.venue();
      sig.status = SignalRejectionReason::VenueNotSynced;
      sig.buy_top_ticks = b_ask->price;
      sig.sell_top_ticks = s_bid->price;
      sig.gross_spread_bps =
          (static_cast<double>(s_bid->price - b_ask->price) / static_cast<double>(b_ask->price)) * 10'000.0;
      sig.buy_quote_age_ns = buy_ad.quote_age_ns(now_ns);
      sig.sell_quote_age_ns = sell_ad.quote_age_ns(now_ns);
      return sig;
    }
    return std::nullopt;
  }

  if (buy_ad.book().is_crossed() || sell_ad.book().is_crossed()) {
    if (has_gross_cross) {
      stats_.gross_crosses_seen += 1;
      stats_.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::CrossedBook)] += 1;
      ArbitrageSignal sig;
      sig.signal_id = next_signal_id_++;
      sig.detect_ns = now_ns;
      sig.instrument_id = inst.id;
      sig.buy_venue = buy_ad.venue();
      sig.sell_venue = sell_ad.venue();
      sig.status = SignalRejectionReason::CrossedBook;
      sig.buy_top_ticks = b_ask->price;
      sig.sell_top_ticks = s_bid->price;
      sig.gross_spread_bps =
          (static_cast<double>(s_bid->price - b_ask->price) / static_cast<double>(b_ask->price)) * 10'000.0;
      sig.buy_quote_age_ns = buy_ad.quote_age_ns(now_ns);
      sig.sell_quote_age_ns = sell_ad.quote_age_ns(now_ns);
      return sig;
    }
    return std::nullopt;
  }

  if (!buy_tradable || !sell_tradable) {
    if (has_gross_cross) {
      stats_.gross_crosses_seen += 1;
      stats_.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::StaleQuote)] += 1;
      ArbitrageSignal sig;
      sig.signal_id = next_signal_id_++;
      sig.detect_ns = now_ns;
      sig.instrument_id = inst.id;
      sig.buy_venue = buy_ad.venue();
      sig.sell_venue = sell_ad.venue();
      sig.status = SignalRejectionReason::StaleQuote;
      sig.buy_top_ticks = b_ask->price;
      sig.sell_top_ticks = s_bid->price;
      sig.gross_spread_bps =
          (static_cast<double>(s_bid->price - b_ask->price) / static_cast<double>(b_ask->price)) * 10'000.0;
      sig.buy_quote_age_ns = buy_ad.quote_age_ns(now_ns);
      sig.sell_quote_age_ns = sell_ad.quote_age_ns(now_ns);
      return sig;
    }
    return std::nullopt;
  }

  if (!has_gross_cross) {
    return std::nullopt;
  }

  stats_.gross_crosses_seen += 1;

  ArbitrageSignal sig;
  sig.signal_id = next_signal_id_++;
  sig.detect_ns = now_ns;
  sig.instrument_id = inst.id;
  sig.buy_venue = buy_ad.venue();
  sig.sell_venue = sell_ad.venue();
  sig.buy_top_ticks = b_ask->price;
  sig.sell_top_ticks = s_bid->price;
  sig.buy_quote_age_ns = buy_ad.quote_age_ns(now_ns);
  sig.sell_quote_age_ns = sell_ad.quote_age_ns(now_ns);
  sig.gross_spread_bps =
      (static_cast<double>(s_bid->price - b_ask->price) / static_cast<double>(b_ask->price)) * 10'000.0;

  const auto& buy_cfg = inst.venues[static_cast<size_t>(buy_ad.venue())];
  const auto& sell_cfg = inst.venues[static_cast<size_t>(sell_ad.venue())];
  const double buy_fee_bps = buy_cfg.taker_fee_bps;
  const double sell_fee_bps = sell_cfg.taker_fee_bps;
  const double required_marginal_bps = cfg_.min_net_edge_bps + cfg_.slippage_buffer_bps;

  // Risk max trade lots from max_trade_notional_quote and optional max_trade_lots.
  const double top_ask_px = inst.ticks_to_price(b_ask->price);
  QtyLots risk_cap_lots =
      (top_ask_px > 0.0) ? static_cast<QtyLots>(std::floor((cfg_.max_trade_notional_quote / top_ask_px) / inst.lot_size))
                         : 0;
  if (cfg_.max_trade_lots > 0) {
    risk_cap_lots = std::min(risk_cap_lots, cfg_.max_trade_lots);
  }

  // First compute depth-only profitable quantity (unconstrained by funding) to distinguish
  // insufficient edge/depth from insufficient pre-funded venue balance.
  const auto depth_cross = compute_executable_cross(buy_ad.book(), sell_ad.book(), buy_fee_bps, sell_fee_bps,
                                                    required_marginal_bps, risk_cap_lots);
  sig.depth_executable_lots = depth_cross.executable_lots;

  const QtyLots common_lot_step = std::max(buy_cfg.lot_step, sell_cfg.lot_step);
  const QtyLots min_lots = std::max(buy_cfg.min_order_lots, sell_cfg.min_order_lots);

  if (depth_cross.executable_lots <= 0) {
    sig.status = SignalRejectionReason::InsufficientEdgeAfterFees;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  if (depth_cross.executable_lots < min_lots) {
    sig.status = SignalRejectionReason::InsufficientDepth;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  // Apply pre-funded venue inventory constraints (no instant transfers!).
  // Quote needed at buy venue includes taker fee.
  const double worst_buy_px = inst.ticks_to_price(depth_cross.buy_worst_ticks);
  const double eff_buy_px = worst_buy_px * (1.0 + buy_fee_bps / 10'000.0);
  const QtyLots quote_funded_lots =
      (eff_buy_px > 0.0 && funding.buy_venue_quote_available > 0.0)
          ? static_cast<QtyLots>(std::floor((funding.buy_venue_quote_available / eff_buy_px) / inst.lot_size))
          : 0;
  const QtyLots base_funded_lots =
      (funding.sell_venue_base_available > 0.0)
          ? static_cast<QtyLots>(std::floor(funding.sell_venue_base_available / inst.lot_size))
          : 0;

  QtyLots funded_cap = std::min({risk_cap_lots, quote_funded_lots, base_funded_lots});
  if (common_lot_step > 1) {
    funded_cap = (funded_cap / common_lot_step) * common_lot_step;
  }

  if (funded_cap < min_lots) {
    sig.status = SignalRejectionReason::InsufficientFunding;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  // Re-evaluate exact depth-aware VWAP at funded_cap!
  const auto funded_cross = compute_executable_cross(buy_ad.book(), sell_ad.book(), buy_fee_bps, sell_fee_bps,
                                                     required_marginal_bps, funded_cap);
  QtyLots final_lots = funded_cross.executable_lots;
  if (common_lot_step > 1) {
    final_lots = (final_lots / common_lot_step) * common_lot_step;
  }
  if (final_lots < min_lots) {
    sig.status = SignalRejectionReason::InsufficientDepth;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  // Recompute VWAP for the snapped lot size if snapping reduced it.
  const auto final_cross =
      (final_lots == funded_cross.executable_lots)
          ? funded_cross
          : compute_executable_cross(buy_ad.book(), sell_ad.book(), buy_fee_bps, sell_fee_bps, required_marginal_bps,
                                     final_lots);

  sig.funded_executable_lots = final_cross.executable_lots;
  sig.buy_vwap_ticks = final_cross.buy_vwap_ticks;
  sig.sell_vwap_ticks = final_cross.sell_vwap_ticks;
  sig.buy_limit_ticks = final_cross.buy_worst_ticks;
  sig.sell_limit_ticks = final_cross.sell_worst_ticks;
  sig.buy_levels_used = final_cross.buy_levels_used;
  sig.sell_levels_used = final_cross.sell_levels_used;
  sig.net_edge_bps = final_cross.net_vwap_edge_bps;

  const double qty_base = inst.lots_to_qty(sig.funded_executable_lots);
  const double buy_notional = qty_base * (sig.buy_vwap_ticks * inst.tick_size);
  const double sell_notional = qty_base * (sig.sell_vwap_ticks * inst.tick_size);
  const double buy_fee = buy_notional * (buy_fee_bps / 10'000.0);
  const double sell_fee = sell_notional * (sell_fee_bps / 10'000.0);
  sig.estimated_fees_quote = buy_fee + sell_fee;
  sig.estimated_net_profit_quote = (sell_notional - sell_fee) - (buy_notional + buy_fee);

  if (sig.net_edge_bps < cfg_.min_net_edge_bps) {
    sig.status = SignalRejectionReason::InsufficientEdgeAfterFees;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  if (sig.estimated_net_profit_quote < cfg_.min_net_profit_quote) {
    sig.status = SignalRejectionReason::BelowMinProfit;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  const size_t ridx = route_index(inst.id, buy_ad.venue(), sell_ad.venue());
  if (last_fire_ns_[ridx] > 0 && now_ns - last_fire_ns_[ridx] < cfg_.cooldown_ns) {
    sig.status = SignalRejectionReason::CooldownActive;
    stats_.rejections_by_reason[static_cast<size_t>(sig.status)] += 1;
    return sig;
  }

  last_fire_ns_[ridx] = now_ns;
  sig.status = SignalRejectionReason::Actionable;
  stats_.actionable_signals += 1;
  stats_.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::Actionable)] += 1;
  return sig;
}

}  // namespace xarb
