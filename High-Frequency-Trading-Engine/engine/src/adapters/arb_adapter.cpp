#include "hft/adapters/arb_adapter.hpp"

#include <algorithm>
#include <cmath>

namespace hft::adapters {

using namespace contracts::v1;

xarb::VenueId PaperArbitrageAdapter::to_xarb_venue(VenueId v) noexcept {
  return static_cast<xarb::VenueId>(v % xarb::kVenueCount);
}

xarb::FlatOrderBook PaperArbitrageAdapter::to_xarb_book(const VenueSnapshotInput& snap) {
  xarb::FlatOrderBook book(32);
  std::vector<xarb::PriceLevel> bids;
  std::vector<xarb::PriceLevel> asks;
  bids.reserve(snap.bids.size());
  asks.reserve(snap.asks.size());
  for (const auto& lv : snap.bids) {
    bids.push_back(xarb::PriceLevel{lv.price.ticks, static_cast<xarb::QtyLots>(lv.qty.lots)});
  }
  for (const auto& lv : snap.asks) {
    asks.push_back(xarb::PriceLevel{lv.price.ticks, static_cast<xarb::QtyLots>(lv.qty.lots)});
  }
  book.apply_snapshot(bids, asks);
  return book;
}

PaperArbitrageAdapter::PaperArbitrageAdapter(const InstrumentMeta& instrument,
                                             const std::vector<VenueMeta>& venues, Config cfg)
    : instrument_(instrument), cfg_(cfg) {
  xarb::InstrumentSpec spec{};
  spec.canonical_symbol = instrument_.symbol_str();
  spec.base_asset = instrument_.symbol_str();
  spec.quote_asset = "USD";
  // tick_size_price4 = 100 -> $0.01 (price_decimals = 2); tick_size_price4 = 1 -> $0.0001 (4)
  spec.price_decimals = (instrument_.tick_size_price4 == 100) ? 2 : 4;
  spec.qty_decimals = 0;  // 1 lot = 1 share/contract in canonical equity simulation
  for (std::size_t i = 0; i < xarb::kVenueCount; ++i) {
    spec.venue_enabled[i] = (i < venues.size() && venues[i].enabled);
    if (i < venues.size()) {
      spec.venues[i].native_symbol = instrument_.symbol_str();
      spec.venues[i].taker_fee_bps =
          std::max(0.0, static_cast<double>(venues[i].taker_fee_bps_x100) / 100.0);
      spec.venues[i].maker_fee_bps =
          std::max(0.0, static_cast<double>(venues[i].maker_fee_bps_x100) / 100.0);
      spec.venues[i].tick_step = 1;
      spec.venues[i].lot_step = 1;
      spec.venues[i].min_order_lots = 1;
    }
  }
  registry_.register_instrument(std::move(spec));

  xarb::PaperConfig pcfg{};
  pcfg.initial_quote_per_venue = cfg_.initial_quote_per_venue;
  pcfg.initial_base_per_venue = cfg_.initial_base_per_venue;
  pcfg.order_latency_ns = cfg_.leg_latency_ns;
  pcfg.order_jitter_ns = 0;
  pcfg.use_limit_ioc = cfg_.use_limit_ioc;
  executor_ = std::make_unique<xarb::PaperExecutor>(registry_, pcfg);
}

std::vector<PaperArbitrageAdapter::EvaluationOutcome>
PaperArbitrageAdapter::evaluate_observed_snapshots(
    SimTimeNs eval_time_ns, const std::vector<VenueSnapshotInput>& observed_snapshots) {
  ++stats_.scans;
  std::vector<EvaluationOutcome> out;
  const xarb::InstrumentSpec* spec = registry_.get(0);
  if (spec == nullptr) return out;

  std::array<xarb::FlatOrderBook, xarb::kVenueCount> books{
      xarb::FlatOrderBook(32), xarb::FlatOrderBook(32), xarb::FlatOrderBook(32)};
  std::array<const VenueSnapshotInput*, xarb::kVenueCount> snaps{};
  for (const auto& s : observed_snapshots) {
    if (s.venue >= xarb::kVenueCount) continue;
    books[s.venue] = to_xarb_book(s);
    snaps[s.venue] = &s;
  }

  for (std::size_t b = 0; b < xarb::kVenueCount; ++b) {
    for (std::size_t s = 0; s < xarb::kVenueCount; ++s) {
      if (b == s || snaps[b] == nullptr || snaps[s] == nullptr) continue;

      const auto best_ask_b = books[b].best_ask();
      const auto best_bid_s = books[s].best_bid();
      if (!best_ask_b.has_value() || !best_bid_s.has_value()) continue;
      if (best_bid_s->price <= best_ask_b->price) continue;

      // Positive gross top-of-book spread observed!
      ++stats_.gross_crosses;

      EvaluationOutcome ev{};
      ev.eval_time_ns = eval_time_ns;
      ev.buy_venue = static_cast<VenueId>(b);
      ev.sell_venue = static_cast<VenueId>(s);
      ev.gross_spread_bps =
          (static_cast<double>(best_bid_s->price - best_ask_b->price) /
           static_cast<double>(best_ask_b->price)) *
          10'000.0;

      // 1. Sync & staleness gates
      const std::int64_t age_b = eval_time_ns.value - snaps[b]->last_update_ns.value;
      const std::int64_t age_s = eval_time_ns.value - snaps[s]->last_update_ns.value;
      if (!snaps[b]->synced || !snaps[s]->synced) {
        ev.status = xarb::SignalRejectionReason::VenueNotSynced;
        ev.gross_positive_net_rejected = true;
        ++stats_.gross_positive_net_infeasible;
        ++stats_.rejected_stale;
        out.push_back(ev);
        continue;
      }
      if (age_b > cfg_.max_stale_ns || age_s > cfg_.max_stale_ns) {
        ev.status = xarb::SignalRejectionReason::StaleQuote;
        ev.gross_positive_net_rejected = true;
        ++stats_.gross_positive_net_infeasible;
        ++stats_.rejected_stale;
        out.push_back(ev);
        continue;
      }

      // 2. Depth-walk & taker-fee feasibility via Project 10 compute_executable_cross
      const double buy_fee_bps = spec->venues[b].taker_fee_bps;
      const double sell_fee_bps = spec->venues[s].taker_fee_bps;
      const double min_marginal = cfg_.min_net_edge_bps + cfg_.slippage_buffer_bps;
      const auto cross = xarb::compute_executable_cross(books[b], books[s], buy_fee_bps,
                                                        sell_fee_bps, min_marginal, 1000);
      ev.net_edge_bps = cross.net_vwap_edge_bps;

      if (cross.executable_lots <= 0) {
        ev.status = (ev.gross_spread_bps <= buy_fee_bps + sell_fee_bps + min_marginal)
                        ? xarb::SignalRejectionReason::InsufficientEdgeAfterFees
                        : xarb::SignalRejectionReason::InsufficientDepth;
        ev.gross_positive_net_rejected = true;
        ++stats_.gross_positive_net_infeasible;
        if (ev.status == xarb::SignalRejectionReason::InsufficientEdgeAfterFees) {
          ++stats_.rejected_fees;
        } else {
          ++stats_.rejected_depth;
        }
        out.push_back(ev);
        continue;
      }

      // 3. Venue funding gate via Project 10 PaperExecutor balances
      const auto funding = executor_->funding_for_route(0, to_xarb_venue(ev.buy_venue),
                                                        to_xarb_venue(ev.sell_venue));
      const double buy_px_q = cross.buy_vwap_ticks * spec->tick_size;
      const double sell_px_q = cross.sell_vwap_ticks * spec->tick_size;
      const double max_lots_by_quote =
          buy_px_q > 0.0 ? std::floor(funding.buy_venue_quote_available /
                                      (buy_px_q * (1.0 + buy_fee_bps / 10'000.0)))
                         : 0.0;
      const double max_lots_by_base = std::floor(funding.sell_venue_base_available);
      const xarb::QtyLots funded_lots = std::min<xarb::QtyLots>(
          cross.executable_lots,
          static_cast<xarb::QtyLots>(std::max(0.0, std::min(max_lots_by_quote, max_lots_by_base))));
      if (funded_lots < 1) {
        ev.status = xarb::SignalRejectionReason::InsufficientFunding;
        ev.gross_positive_net_rejected = true;
        ++stats_.gross_positive_net_infeasible;
        ++stats_.rejected_funding;
        out.push_back(ev);
        continue;
      }

      const double qty = static_cast<double>(funded_lots) * spec->lot_size;
      const double fees_q =
          qty * (buy_px_q * buy_fee_bps / 10'000.0 + sell_px_q * sell_fee_bps / 10'000.0);
      const double net_profit_q = qty * (sell_px_q - buy_px_q) - fees_q;
      ev.executable_lots = static_cast<std::uint64_t>(funded_lots);
      ev.estimated_fees_quote = fees_q;
      ev.estimated_net_profit_quote = net_profit_q;

      if (net_profit_q < cfg_.min_net_profit_quote) {
        ev.status = xarb::SignalRejectionReason::BelowMinProfit;
        ev.gross_positive_net_rejected = true;
        ++stats_.gross_positive_net_infeasible;
        ++stats_.rejected_fees;
        out.push_back(ev);
        continue;
      }

      ev.status = xarb::SignalRejectionReason::Actionable;
      ++stats_.actionable_signals;
      out.push_back(ev);
    }
  }
  return out;
}

xarb::ExecutionOutcome PaperArbitrageAdapter::execute_paper_against_delayed_truth(
    const EvaluationOutcome& opp, const std::vector<VenueSnapshotInput>& observed_at_detect,
    const std::vector<VenueSnapshotInput>& truth_at_buy_arrival,
    const std::vector<VenueSnapshotInput>& truth_at_sell_arrival, double mark_mid_quote) {
  if (opp.status != xarb::SignalRejectionReason::Actionable || opp.executable_lots == 0) {
    return xarb::ExecutionOutcome::BothMissedExpired;
  }

  auto find_snap = [](const std::vector<VenueSnapshotInput>& vec,
                      VenueId v) -> const VenueSnapshotInput* {
    for (const auto& s : vec) {
      if (s.venue == v) return &s;
    }
    return nullptr;
  };

  const auto* obs_buy = find_snap(observed_at_detect, opp.buy_venue);
  const auto* obs_sell = find_snap(observed_at_detect, opp.sell_venue);
  const auto* tr_buy = find_snap(truth_at_buy_arrival, opp.buy_venue);
  const auto* tr_sell = find_snap(truth_at_sell_arrival, opp.sell_venue);
  if (!obs_buy || !obs_sell || !tr_buy || !tr_sell) {
    return xarb::ExecutionOutcome::BothMissedExpired;
  }

  const auto obs_buy_book = to_xarb_book(*obs_buy);
  const auto obs_sell_book = to_xarb_book(*obs_sell);
  const auto tr_buy_book = to_xarb_book(*tr_buy);
  const auto tr_sell_book = to_xarb_book(*tr_sell);

  const auto* spec = registry_.get(0);
  const auto cross = xarb::compute_executable_cross(
      obs_buy_book, obs_sell_book, spec->venues[opp.buy_venue].taker_fee_bps,
      spec->venues[opp.sell_venue].taker_fee_bps, cfg_.min_net_edge_bps,
      static_cast<xarb::QtyLots>(opp.executable_lots));

  xarb::ArbitrageSignal sig{};
  sig.signal_id = next_signal_id_++;
  sig.detect_ns = opp.eval_time_ns.value;
  sig.instrument_id = 0;
  sig.buy_venue = to_xarb_venue(opp.buy_venue);
  sig.sell_venue = to_xarb_venue(opp.sell_venue);
  sig.status = xarb::SignalRejectionReason::Actionable;
  sig.buy_top_ticks = obs_buy_book.best_ask().has_value() ? obs_buy_book.best_ask()->price : 0;
  sig.sell_top_ticks = obs_sell_book.best_bid().has_value() ? obs_sell_book.best_bid()->price : 0;
  sig.buy_vwap_ticks = cross.buy_vwap_ticks;
  sig.sell_vwap_ticks = cross.sell_vwap_ticks;
  sig.buy_limit_ticks = cross.buy_worst_ticks;
  sig.sell_limit_ticks = cross.sell_worst_ticks;
  sig.depth_executable_lots = static_cast<xarb::QtyLots>(opp.executable_lots);
  sig.funded_executable_lots = static_cast<xarb::QtyLots>(opp.executable_lots);
  sig.gross_spread_bps = opp.gross_spread_bps;
  sig.net_edge_bps = opp.net_edge_bps;
  sig.estimated_fees_quote = opp.estimated_fees_quote;
  sig.estimated_net_profit_quote = opp.estimated_net_profit_quote;

  const std::int64_t buy_arr = opp.eval_time_ns.value + cfg_.leg_latency_ns[opp.buy_venue];
  const std::int64_t sell_arr = opp.eval_time_ns.value + cfg_.leg_latency_ns[opp.sell_venue];
  const auto legs = executor_->submit_signal(sig, buy_arr, sell_arr);
  if (legs.empty()) return xarb::ExecutionOutcome::RejectedUnfunded;

  for (const auto& leg : legs) {
    const auto& tbook = leg.is_buy ? tr_buy_book : tr_sell_book;
    (void)executor_->execute_leg(leg, tbook, mark_mid_quote);
  }
  if (executor_->trades().empty()) return xarb::ExecutionOutcome::BothMissedExpired;
  return executor_->trades().back().outcome;
}

PaperArbitrageAdapter::PaperSummary PaperArbitrageAdapter::summary(double mark_mid_quote) const {
  PaperSummary out = stats_;
  out.paper_stats = executor_->stats();
  out.invariants_ok = executor_->verify_invariants(0);
  out.initial_equity_quote = executor_->initial_equity_quote(0, mark_mid_quote);
  out.final_equity_quote = executor_->total_equity_quote(0, mark_mid_quote);
  return out;
}

}  // namespace hft::adapters
