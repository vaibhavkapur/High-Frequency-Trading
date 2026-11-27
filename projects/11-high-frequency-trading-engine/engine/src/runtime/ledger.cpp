#include "hft/runtime/ledger.hpp"

#include <algorithm>
#include <cstdlib>

namespace hft::runtime {

using namespace contracts::v1;

AccountingLedger::AccountingLedger(std::vector<InstrumentMeta> instruments, Money initial_cash)
    : instruments_(std::move(instruments)),
      initial_cash_(initial_cash),
      total_cash_(initial_cash),
      by_instrument_(instruments_.size()) {}

bool AccountingLedger::on_fill(VenueId venue, InstrumentId instrument,
                               const ExecutionReport& report) {
  if (instrument >= instruments_.size()) return false;
  if (report.fill_qty.lots == 0 || report.exec_id == 0) return false;

  const std::uint64_t exec_key = make_exec_key(venue, report.exec_id);
  if (!seen_exec_keys_.insert(exec_key).second) {
    ++duplicate_exec_ids_ignored_;
    return false;
  }

  const auto& inst = instruments_[instrument];
  const auto notional_opt = inst.notional_scaled(report.exec_price, report.fill_qty);
  if (!notional_opt.has_value()) return false;
  const Money notional = *notional_opt;
  const Money fee = report.fee_scaled;

  InstrumentLedgerState& st = by_instrument_[instrument];
  const std::int64_t q = static_cast<std::int64_t>(report.fill_qty.lots);
  const std::int64_t old_pos = st.position_lots;
  const std::int64_t signed_q = (report.side == Side::Buy) ? q : -q;
  const Money cash_delta =
      (report.side == Side::Buy) ? Money{-notional.scaled - fee.scaled}
                                 : Money{+notional.scaled - fee.scaled};

  // Exact integer cost-basis accounting:
  // Invariant maintained at every step:
  //   cash_ex_fees + cost_basis_scaled == realized_pnl_scaled
  const std::int64_t per_lot_scaled = notional.scaled / q;  // exact because notional = px4*lot*q

  if (old_pos == 0 || (old_pos > 0 && signed_q > 0) || (old_pos < 0 && signed_q < 0)) {
    // Opening or adding to existing direction.
    st.position_lots = old_pos + signed_q;
    st.cost_basis_scaled.scaled += (report.side == Side::Buy) ? notional.scaled : -notional.scaled;
  } else {
    // Reducing or flipping existing position.
    const std::int64_t abs_old = std::abs(old_pos);
    const std::int64_t close_lots = std::min(abs_old, q);
    const std::int64_t open_lots = q - close_lots;

    const __int128 closed_basis_128 =
        (close_lots == abs_old)
            ? static_cast<__int128>(st.cost_basis_scaled.scaled)
            : (static_cast<__int128>(st.cost_basis_scaled.scaled) *
               static_cast<__int128>(close_lots)) /
                  static_cast<__int128>(abs_old);
    const std::int64_t closed_basis = static_cast<std::int64_t>(closed_basis_128);
    const std::int64_t close_notional = per_lot_scaled * close_lots;

    if (old_pos > 0) {
      // Selling to close long: proceeds (+close_notional) minus cost basis (+closed_basis).
      st.realized_pnl_scaled.scaled += (close_notional - closed_basis);
    } else {
      // Buying to cover short: short proceeds (-closed_basis) minus cover cost (+close_notional).
      st.realized_pnl_scaled.scaled += (-closed_basis - close_notional);
    }
    st.cost_basis_scaled.scaled -= closed_basis;

    if (open_lots > 0) {
      const std::int64_t open_notional = per_lot_scaled * open_lots;
      st.cost_basis_scaled.scaled +=
          (report.side == Side::Buy) ? open_notional : -open_notional;
    }
    st.position_lots = old_pos + signed_q;
  }

  st.cash_scaled += cash_delta;
  st.fees_scaled += fee;
  st.last_mark_price = report.exec_price;
  st.traded_lots += report.fill_qty.lots;
  st.fill_count += 1;
  total_cash_ += cash_delta;
  venue_positions_[make_vi_key(venue, instrument)] += signed_q;

  ExecutedFillRecord rec{};
  rec.venue = venue;
  rec.instrument = instrument;
  rec.exec_id = report.exec_id;
  rec.token = report.token;
  rec.side = report.side;
  rec.price = report.exec_price;
  rec.qty = report.fill_qty;
  rec.notional_scaled = notional;
  rec.fee_scaled = fee;
  rec.cash_delta_scaled = cash_delta;
  rec.time_ns = report.exchange_time_ns;
  fills_.push_back(rec);
  return true;
}

void AccountingLedger::update_mark(InstrumentId instrument, Price mark_price) {
  if (instrument >= by_instrument_.size() || !mark_price.valid()) return;
  by_instrument_[instrument].last_mark_price = mark_price;
}

std::int64_t AccountingLedger::net_position(InstrumentId instrument) const noexcept {
  if (instrument >= by_instrument_.size()) return 0;
  return by_instrument_[instrument].position_lots;
}

std::int64_t AccountingLedger::venue_position(VenueId venue,
                                              InstrumentId instrument) const noexcept {
  const auto it = venue_positions_.find(make_vi_key(venue, instrument));
  return it == venue_positions_.end() ? 0 : it->second;
}

const InstrumentLedgerState& AccountingLedger::instrument_state(InstrumentId instrument) const {
  return by_instrument_.at(instrument);
}

ReconciliationReport AccountingLedger::reconcile() const {
  ReconciliationReport rep{};
  rep.initial_cash = initial_cash_;
  rep.cash_scaled = total_cash_;
  rep.unique_fills = fills_.size();
  rep.duplicate_exec_ids_ignored = duplicate_exec_ids_ignored_;

  // 1. Recompute cash, fees, and per-instrument positions independently from raw execution history.
  Money history_cash = initial_cash_;
  Money history_fees{0};
  std::vector<std::int64_t> history_pos(instruments_.size(), 0);
  for (const auto& f : fills_) {
    history_cash += f.cash_delta_scaled;
    history_fees += f.fee_scaled;
    const std::int64_t q = static_cast<std::int64_t>(f.qty.lots);
    history_pos[f.instrument] += (f.side == Side::Buy) ? q : -q;
  }

  // 2. Compute marked inventory value, realized P&L, unrealized P&L, and total fees.
  Money mark_inv{0};
  Money realized{0};
  Money unrealized{0};
  Money ledger_fees{0};

  for (std::size_t i = 0; i < by_instrument_.size(); ++i) {
    const auto& st = by_instrument_[i];
    if (st.position_lots != history_pos[i]) {
      rep.ok = false;
      rep.failure_reason = "position mismatch between ledger and execution history";
      return rep;
    }
    ledger_fees += st.fees_scaled;
    realized += st.realized_pnl_scaled;

    Money inst_mark_val{0};
    if (st.position_lots != 0 && st.last_mark_price.valid()) {
      const auto abs_q = Quantity{static_cast<std::uint64_t>(std::abs(st.position_lots))};
      const auto mv = instruments_[i].notional_scaled(st.last_mark_price, abs_q);
      if (mv.has_value()) {
        inst_mark_val.scaled = (st.position_lots > 0) ? mv->scaled : -mv->scaled;
      }
    }
    mark_inv += inst_mark_val;
    unrealized.scaled += (inst_mark_val.scaled - st.cost_basis_scaled.scaled);
  }

  rep.mark_inventory_value = mark_inv;
  rep.total_fees = ledger_fees;
  rep.realized_pnl = realized;
  rep.unrealized_pnl = unrealized;
  rep.net_equity_cash_plus_inv = total_cash_ + mark_inv;
  rep.net_equity_pnl_decomp = initial_cash_ + realized + unrealized - ledger_fees;
  rep.discrepancy_scaled =
      rep.net_equity_cash_plus_inv.scaled - rep.net_equity_pnl_decomp.scaled;

  if (history_cash != total_cash_) {
    rep.ok = false;
    rep.failure_reason = "cash mismatch between ledger and execution history";
    return rep;
  }
  if (history_fees != ledger_fees) {
    rep.ok = false;
    rep.failure_reason = "fee mismatch between ledger and execution history";
    return rep;
  }
  if (rep.discrepancy_scaled != 0) {
    rep.ok = false;
    rep.failure_reason = "equity conservation discrepancy (cash+inv != initial+realized+unrealized-fees)";
    return rep;
  }

  // 3. Cross-check sum of per-venue positions equals per-instrument net positions.
  std::vector<std::int64_t> venue_sum(instruments_.size(), 0);
  for (const auto& [k, pos] : venue_positions_) {
    const auto inst = static_cast<InstrumentId>(k & 0xFFFFFFFFULL);
    if (inst < venue_sum.size()) venue_sum[inst] += pos;
  }
  for (std::size_t i = 0; i < by_instrument_.size(); ++i) {
    if (venue_sum[i] != by_instrument_[i].position_lots) {
      rep.ok = false;
      rep.failure_reason = "sum of venue positions does not match net instrument position";
      return rep;
    }
  }

  rep.ok = true;
  return rep;
}

}  // namespace hft::runtime
