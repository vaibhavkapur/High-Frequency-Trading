#include "xarb/paper_executor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xarb {

PaperExecutor::PaperExecutor(const InstrumentRegistry& registry, PaperConfig cfg)
    : registry_(registry), cfg_(cfg) {
  const size_t n = registry_.size();
  balances_.resize(n);
  initial_total_quote_.assign(n, 0.0);
  initial_total_base_.assign(n, 0.0);
  cumulative_base_rebalance_fees_.assign(n, 0.0);
  in_flight_rebalance_base_.assign(n, 0.0);

  for (size_t i = 0; i < n; ++i) {
    for (size_t v = 0; v < kVenueCount; ++v) {
      balances_[i][v].quote_available = cfg_.initial_quote_per_venue[v];
      balances_[i][v].base_available = cfg_.initial_base_per_venue[v];
      initial_total_quote_[i] += cfg_.initial_quote_per_venue[v];
      initial_total_base_[i] += cfg_.initial_base_per_venue[v];
    }
  }
}

const VenueBalance& PaperExecutor::balance(uint32_t inst_id, VenueId v) const {
  return balances_.at(inst_id)[static_cast<size_t>(v)];
}

RouteFundingLimits PaperExecutor::funding_for_route(uint32_t inst_id, VenueId buy_v, VenueId sell_v) const {
  RouteFundingLimits lim;
  if (inst_id < balances_.size()) {
    lim.buy_venue_quote_available = balances_[inst_id][static_cast<size_t>(buy_v)].quote_available;
    lim.sell_venue_base_available = balances_[inst_id][static_cast<size_t>(sell_v)].base_available;
  }
  return lim;
}

std::vector<PendingLegOrder> PaperExecutor::submit_signal(const ArbitrageSignal& sig, TimestampNs buy_arrival_ns,
                                                          TimestampNs sell_arrival_ns) {
  std::vector<PendingLegOrder> legs;
  const InstrumentSpec* inst = registry_.get(sig.instrument_id);
  if (!inst || !sig.is_actionable() || sig.funded_executable_lots <= 0) {
    return legs;
  }

  auto& buy_bal = balances_[sig.instrument_id][static_cast<size_t>(sig.buy_venue)];
  auto& sell_bal = balances_[sig.instrument_id][static_cast<size_t>(sig.sell_venue)];

  const double qty_base = inst->lots_to_qty(sig.funded_executable_lots);
  const double buy_fee_rate = inst->venues[static_cast<size_t>(sig.buy_venue)].taker_fee_bps / 10'000.0;

  PriceTicks buy_limit_ticks = 0;
  PriceTicks sell_limit_ticks = 0;
  if (cfg_.use_limit_ioc) {
    const double tol = cfg_.limit_slippage_tolerance_bps / 10'000.0;
    buy_limit_ticks = static_cast<PriceTicks>(std::ceil(static_cast<double>(sig.buy_limit_ticks) * (1.0 + tol)));
    sell_limit_ticks = static_cast<PriceTicks>(std::floor(static_cast<double>(sig.sell_limit_ticks) * (1.0 - tol)));
  }

  // Reserve worst-case quote needed for buy leg (plus 2% buffer if market IOC).
  const double reserve_px = cfg_.use_limit_ioc
                                ? inst->ticks_to_price(buy_limit_ticks)
                                : inst->ticks_to_price(sig.buy_limit_ticks) * 1.02;
  const double required_quote = qty_base * reserve_px * (1.0 + buy_fee_rate);

  stats_.trades_attempted += 1;

  if (buy_bal.quote_available + 1e-9 < required_quote || sell_bal.base_available + 1e-12 < qty_base) {
    stats_.rejected_unfunded += 1;
    PaperTradeRecord rec;
    rec.trade_id = next_trade_id_++;
    rec.signal_id = sig.signal_id;
    rec.instrument_id = sig.instrument_id;
    rec.buy_venue = sig.buy_venue;
    rec.sell_venue = sig.sell_venue;
    rec.detect_ns = sig.detect_ns;
    rec.target_lots = sig.funded_executable_lots;
    rec.expected_buy_vwap = sig.buy_vwap_ticks;
    rec.expected_sell_vwap = sig.sell_vwap_ticks;
    rec.expected_net_profit_quote = sig.estimated_net_profit_quote;
    rec.expected_net_edge_bps = sig.net_edge_bps;
    rec.gross_spread_bps = sig.gross_spread_bps;
    rec.outcome = ExecutionOutcome::RejectedUnfunded;
    trades_.push_back(rec);
    return legs;
  }

  // Reserve balances atomically before legs are in flight.
  buy_bal.quote_available -= required_quote;
  buy_bal.quote_reserved += required_quote;
  sell_bal.base_available -= qty_base;
  sell_bal.base_reserved += qty_base;

  const uint64_t tid = next_trade_id_++;
  PaperTradeRecord rec;
  rec.trade_id = tid;
  rec.signal_id = sig.signal_id;
  rec.instrument_id = sig.instrument_id;
  rec.buy_venue = sig.buy_venue;
  rec.sell_venue = sig.sell_venue;
  rec.detect_ns = sig.detect_ns;
  rec.buy_arrival_ns = buy_arrival_ns;
  rec.sell_arrival_ns = sell_arrival_ns;
  rec.target_lots = sig.funded_executable_lots;
  rec.expected_buy_vwap = sig.buy_vwap_ticks;
  rec.expected_sell_vwap = sig.sell_vwap_ticks;
  rec.expected_net_profit_quote = sig.estimated_net_profit_quote;
  rec.expected_net_edge_bps = sig.net_edge_bps;
  rec.gross_spread_bps = sig.gross_spread_bps;
  rec.outcome = ExecutionOutcome::Pending;

  trade_index_[tid] = trades_.size();
  trades_.push_back(rec);

  PendingLegOrder buy_leg;
  buy_leg.trade_id = tid;
  buy_leg.instrument_id = sig.instrument_id;
  buy_leg.venue = sig.buy_venue;
  buy_leg.is_buy = true;
  buy_leg.submit_ns = sig.detect_ns;
  buy_leg.arrival_ns = buy_arrival_ns;
  buy_leg.target_lots = sig.funded_executable_lots;
  buy_leg.limit_ticks = buy_limit_ticks;
  buy_leg.reserved_amount = required_quote;

  PendingLegOrder sell_leg;
  sell_leg.trade_id = tid;
  sell_leg.instrument_id = sig.instrument_id;
  sell_leg.venue = sig.sell_venue;
  sell_leg.is_buy = false;
  sell_leg.submit_ns = sig.detect_ns;
  sell_leg.arrival_ns = sell_arrival_ns;
  sell_leg.target_lots = sig.funded_executable_lots;
  sell_leg.limit_ticks = sell_limit_ticks;
  sell_leg.reserved_amount = qty_base;

  legs.push_back(buy_leg);
  legs.push_back(sell_leg);
  return legs;
}

bool PaperExecutor::execute_leg(const PendingLegOrder& leg, const FlatOrderBook& truth_book,
                                double mark_mid_price_quote) {
  auto it = trade_index_.find(leg.trade_id);
  if (it == trade_index_.end()) return false;
  PaperTradeRecord& tr = trades_[it->second];
  const InstrumentSpec* inst = registry_.get(leg.instrument_id);
  if (!inst) return false;

  auto& bal = balances_[leg.instrument_id][static_cast<size_t>(leg.venue)];
  const double fee_rate = inst->venues[static_cast<size_t>(leg.venue)].taker_fee_bps / 10'000.0;

  if (leg.is_buy) {
    // Release reservation first, then settle exact executed notional + fee.
    bal.quote_reserved = std::max(0.0, bal.quote_reserved - leg.reserved_amount);
    bal.quote_available += leg.reserved_amount;

    const auto fill = truth_book.walk_asks(leg.target_lots, leg.limit_ticks);
    QtyLots exec_lots = fill.filled_lots;
    double exec_vwap_ticks = fill.vwap_ticks;

    // Guard against market-IOC exceeding available quote if book jumped dramatically.
    if (exec_lots > 0) {
      const double qty_base = inst->lots_to_qty(exec_lots);
      const double cost_quote = qty_base * (exec_vwap_ticks * inst->tick_size);
      const double fee_quote = cost_quote * fee_rate;
      const double total_spend = cost_quote + fee_quote;
      if (total_spend > bal.quote_available + 1e-6) {
        // Scale down to affordable lots.
        const double ratio = bal.quote_available / total_spend;
        exec_lots = static_cast<QtyLots>(std::floor(static_cast<double>(exec_lots) * ratio));
        const auto refilled = truth_book.walk_asks(exec_lots, leg.limit_ticks);
        exec_lots = refilled.filled_lots;
        exec_vwap_ticks = refilled.vwap_ticks;
      }
    }

    if (exec_lots > 0) {
      const double qty_base = inst->lots_to_qty(exec_lots);
      const double cost_quote = qty_base * (exec_vwap_ticks * inst->tick_size);
      const double fee_quote = cost_quote * fee_rate;
      bal.quote_available -= (cost_quote + fee_quote);
      bal.base_available += qty_base;
      bal.total_fees_paid_quote += fee_quote;
      bal.cumulative_bought_base += qty_base;

      tr.buy_filled_lots = exec_lots;
      tr.buy_exec_vwap = exec_vwap_ticks;
      tr.buy_fee_quote = fee_quote;
      stats_.total_turnover_quote += cost_quote;
      stats_.total_fees_quote += fee_quote;
    }
    tr.buy_done = true;
  } else {
    bal.base_reserved = std::max(0.0, bal.base_reserved - leg.reserved_amount);
    bal.base_available += leg.reserved_amount;

    const auto fill = truth_book.walk_bids(leg.target_lots, leg.limit_ticks);
    const QtyLots exec_lots = fill.filled_lots;
    if (exec_lots > 0) {
      const double qty_base = inst->lots_to_qty(exec_lots);
      const double proceeds_quote = qty_base * (fill.vwap_ticks * inst->tick_size);
      const double fee_quote = proceeds_quote * fee_rate;
      bal.base_available -= qty_base;
      bal.quote_available += (proceeds_quote - fee_quote);
      bal.total_fees_paid_quote += fee_quote;
      bal.cumulative_sold_base += qty_base;

      tr.sell_filled_lots = exec_lots;
      tr.sell_exec_vwap = fill.vwap_ticks;
      tr.sell_fee_quote = fee_quote;
      stats_.total_turnover_quote += proceeds_quote;
      stats_.total_fees_quote += fee_quote;
    }
    tr.sell_done = true;
  }

  if (tr.buy_done && tr.sell_done) {
    finalize_trade(tr, mark_mid_price_quote);
    return true;
  }
  return false;
}

void PaperExecutor::finalize_trade(PaperTradeRecord& tr, double mark_mid_price_quote) {
  const InstrumentSpec* inst = registry_.get(tr.instrument_id);
  if (!inst) return;

  stats_.trades_completed += 1;

  const QtyLots matched_lots = std::min(tr.buy_filled_lots, tr.sell_filled_lots);
  const double matched_base = inst->lots_to_qty(matched_lots);
  const double buy_base = inst->lots_to_qty(tr.buy_filled_lots);
  const double sell_base = inst->lots_to_qty(tr.sell_filled_lots);
  const double buy_px = tr.buy_exec_vwap * inst->tick_size;
  const double sell_px = tr.sell_exec_vwap * inst->tick_size;

  // Cash flow of the two legs + mark-to-market of any unhedged inventory residual.
  const double cash_flow =
      (sell_base * sell_px - tr.sell_fee_quote) - (buy_base * buy_px + tr.buy_fee_quote);
  tr.unhedged_base_delta = buy_base - sell_base;

  // Split total P&L cleanly into matched arbitrage P&L and unhedged residual MTM.
  if (matched_lots > 0) {
    const double buy_fee_share = (buy_base > 0.0) ? tr.buy_fee_quote * (matched_base / buy_base) : 0.0;
    const double sell_fee_share = (sell_base > 0.0) ? tr.sell_fee_quote * (matched_base / sell_base) : 0.0;
    tr.realized_matched_pnl_quote =
        matched_base * (sell_px - buy_px) - (buy_fee_share + sell_fee_share);
  }
  tr.total_pnl_quote = cash_flow + tr.unhedged_base_delta * mark_mid_price_quote;
  tr.unhedged_mtm_pnl_quote = tr.total_pnl_quote - tr.realized_matched_pnl_quote;
  tr.slippage_vs_expected_quote = tr.expected_net_profit_quote - tr.total_pnl_quote;

  if (tr.buy_filled_lots == 0 && tr.sell_filled_lots == 0) {
    tr.outcome = ExecutionOutcome::BothMissedExpired;
    stats_.both_missed_expired += 1;
  } else if (tr.buy_filled_lots != tr.sell_filled_lots) {
    tr.outcome = ExecutionOutcome::OneLeggedUnhedged;
    stats_.one_legged_unhedged += 1;
  } else if (tr.total_pnl_quote >= 0.0) {
    tr.outcome = ExecutionOutcome::BothFilledProfitable;
    stats_.both_filled_profitable += 1;
  } else {
    tr.outcome = ExecutionOutcome::BothFilledLossSlippage;
    stats_.both_filled_loss_slippage += 1;
  }

  stats_.total_realized_matched_pnl_quote += tr.realized_matched_pnl_quote;
  stats_.total_unhedged_mtm_pnl_quote += tr.unhedged_mtm_pnl_quote;
  stats_.net_simulated_pnl_quote += tr.total_pnl_quote;

  if (stats_.net_simulated_pnl_quote > peak_pnl_) {
    peak_pnl_ = stats_.net_simulated_pnl_quote;
  }
  const double dd = peak_pnl_ - stats_.net_simulated_pnl_quote;
  if (dd > stats_.max_drawdown_quote) {
    stats_.max_drawdown_quote = dd;
  }
}

std::optional<RebalanceTransfer> PaperExecutor::maybe_initiate_rebalance(uint32_t inst_id, TimestampNs now_ns) {
  if (!cfg_.enable_rebalance_model || inst_id >= balances_.size()) return std::nullopt;
  const InstrumentSpec* inst = registry_.get(inst_id);
  if (!inst) return std::nullopt;

  // Find a depleted venue and an excess venue.
  size_t min_v = 0;
  size_t max_v = 0;
  for (size_t v = 1; v < kVenueCount; ++v) {
    if (balances_[inst_id][v].base_available < balances_[inst_id][min_v].base_available) min_v = v;
    if (balances_[inst_id][v].base_available > balances_[inst_id][max_v].base_available) max_v = v;
  }
  if (min_v == max_v) return std::nullopt;

  const double target = cfg_.initial_base_per_venue[min_v];
  if (target <= 0.0 || balances_[inst_id][min_v].base_available >= target * cfg_.rebalance_min_base_ratio) {
    return std::nullopt;
  }
  const double surplus = balances_[inst_id][max_v].base_available - cfg_.initial_base_per_venue[max_v];
  const double fee_base = inst->venues[max_v].withdrawal_fee_base;
  if (surplus <= fee_base * 4.0) return std::nullopt;

  const double send_amount = std::min(surplus * 0.5, target - balances_[inst_id][min_v].base_available);
  if (send_amount <= fee_base) return std::nullopt;

  balances_[inst_id][max_v].base_available -= send_amount;
  in_flight_rebalance_base_[inst_id] += (send_amount - fee_base);
  cumulative_base_rebalance_fees_[inst_id] += fee_base;

  RebalanceTransfer xfer;
  xfer.transfer_id = next_transfer_id_++;
  xfer.instrument_id = inst_id;
  xfer.from_venue = static_cast<VenueId>(max_v);
  xfer.to_venue = static_cast<VenueId>(min_v);
  xfer.amount_base = send_amount - fee_base;
  xfer.fee_base = fee_base;
  xfer.start_ns = now_ns;
  xfer.arrival_ns = now_ns + inst->venues[max_v].transfer_delay_ns;
  stats_.rebalances_initiated += 1;
  return xfer;
}

void PaperExecutor::complete_rebalance(const RebalanceTransfer& xfer, double mark_mid_price_quote) {
  if (xfer.instrument_id >= balances_.size()) return;
  balances_[xfer.instrument_id][static_cast<size_t>(xfer.to_venue)].base_available += xfer.amount_base;
  in_flight_rebalance_base_[xfer.instrument_id] =
      std::max(0.0, in_flight_rebalance_base_[xfer.instrument_id] - xfer.amount_base);
  stats_.rebalances_completed += 1;
  const double fee_q = xfer.fee_base * mark_mid_price_quote;
  stats_.total_rebalance_fees_quote += fee_q;
  stats_.net_simulated_pnl_quote -= fee_q;
}

double PaperExecutor::total_equity_quote(uint32_t inst_id, double mark_mid_price_quote) const {
  if (inst_id >= balances_.size()) return 0.0;
  double total_q = 0.0;
  double total_b = in_flight_rebalance_base_[inst_id];
  for (size_t v = 0; v < kVenueCount; ++v) {
    total_q += balances_[inst_id][v].quote_available + balances_[inst_id][v].quote_reserved;
    total_b += balances_[inst_id][v].base_available + balances_[inst_id][v].base_reserved;
  }
  return total_q + total_b * mark_mid_price_quote;
}

double PaperExecutor::initial_equity_quote(uint32_t inst_id, double mark_mid_price_quote) const {
  if (inst_id >= initial_total_quote_.size()) return 0.0;
  return initial_total_quote_[inst_id] + initial_total_base_[inst_id] * mark_mid_price_quote;
}

bool PaperExecutor::verify_invariants(uint32_t inst_id, double tolerance) const {
  if (inst_id >= balances_.size()) return false;
  double sum_base = in_flight_rebalance_base_[inst_id] + cumulative_base_rebalance_fees_[inst_id];
  double sum_bought = 0.0;
  double sum_sold = 0.0;
  for (size_t v = 0; v < kVenueCount; ++v) {
    const auto& b = balances_[inst_id][v];
    if (b.quote_available < -tolerance || b.quote_reserved < -tolerance ||
        b.base_available < -tolerance || b.base_reserved < -tolerance) {
      return false;
    }
    sum_base += b.base_available + b.base_reserved;
    sum_bought += b.cumulative_bought_base;
    sum_sold += b.cumulative_sold_base;
  }
  const double expected_base = initial_total_base_[inst_id] + sum_bought - sum_sold;
  return std::fabs(sum_base - expected_base) <= tolerance;
}

}  // namespace xarb
