#include "hft/runtime/oms_reconciler.hpp"

namespace hft::runtime {

using namespace contracts::v1;

OmsReconciler::OmsReconciler(AccountingLedger& ledger, adapters::OuchJournalBridge* journal)
    : ledger_(ledger), journal_(journal) {}

bool OmsReconciler::on_order_sent(VenueId venue, InstrumentId instrument,
                                  const OrderIntent& intent, SimTimeNs now_ns) {
  if (intent.action == OrderAction::New) {
    if (orders_.contains(intent.token)) return false;
    ManagedOrder mo{};
    mo.venue = venue;
    mo.instrument = instrument;
    mo.client_order_id = intent.client_order_id;
    mo.token = intent.token;
    mo.side = intent.side;
    mo.price = intent.limit_price;
    mo.submitted_qty = intent.qty;
    mo.cum_filled_qty = Quantity{0};
    mo.leaves_qty = intent.qty;  // Full qty counted in worst-case exposure immediately!
    mo.canceled_qty = Quantity{0};
    mo.state = ManagedOrderState::PendingNew;
    mo.submit_time_ns = now_ns;
    mo.last_update_ns = now_ns;
    orders_[intent.token] = mo;
    ++summary_.submitted;
    if (journal_ != nullptr) {
      journal_->record_order_sent(instrument, intent,
                                  static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
                                  journal_seq_++);
    }
    return true;
  }

  if (intent.action == OrderAction::Cancel) {
    const ClientToken target = intent.orig_token.empty() ? intent.token : intent.orig_token;
    auto it = orders_.find(target);
    if (it == orders_.end() || is_terminal_order_state(it->second.state)) return false;
    // Note: Do NOT release exposure or mark Canceled yet! Only confirmed exchange Canceled
    // releases exposure (lost cancel response is never silently treated as a cancellation).
    it->second.cancel_requested = true;
    if (it->second.state == ManagedOrderState::Live ||
        it->second.state == ManagedOrderState::PartiallyFilled) {
      it->second.state = ManagedOrderState::PendingCancel;
    }
    it->second.last_update_ns = now_ns;
    if (journal_ != nullptr) {
      journal_->record_order_sent(instrument, intent,
                                  static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
                                  journal_seq_++);
    }
    return true;
  }

  return false;
}

bool OmsReconciler::on_execution_report(VenueId venue, InstrumentId instrument,
                                        const ExecutionReport& report, SimTimeNs now_ns) {
  auto it = orders_.find(report.token);
  if (it == orders_.end()) return false;
  ManagedOrder& ord = it->second;

  const bool was_uncertain = (ord.state == ManagedOrderState::Uncertain);

  if (report.type == ExecReportType::PartiallyFilled || report.type == ExecReportType::Filled) {
    if (report.exec_id != 0) {
      const std::uint64_t ek = make_exec_key(venue, report.exec_id);
      if (!seen_exec_ids_.insert(ek).second) {
        ++summary_.duplicate_reports_ignored;
        return false;
      }
    }
    if (ord.cum_filled_qty.lots + report.fill_qty.lots > ord.submitted_qty.lots) {
      return false;
    }
    if (report.exchange_order_id != 0) ord.exchange_order_id = report.exchange_order_id;
    ord.cum_filled_qty.lots += report.fill_qty.lots;
    const std::uint64_t remaining =
        ord.submitted_qty.lots - ord.cum_filled_qty.lots - ord.canceled_qty.lots;
    ord.leaves_qty.lots = remaining;
    ord.state = (remaining == 0) ? ManagedOrderState::Filled : ManagedOrderState::PartiallyFilled;
    ord.last_update_ns = now_ns;
    if (remaining == 0) {
      ++summary_.filled;
    } else {
      ++summary_.partial_fills;
    }
    if (was_uncertain) ++summary_.uncertain_reconciled;
    (void)ledger_.on_fill(venue, instrument, report);
    if (journal_ != nullptr) {
      journal_->record_exec_report(
          instrument, report, static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
          journal_seq_++);
    }
    return true;
  }

  if (report.type == ExecReportType::Accepted) {
    // Ignore duplicate Accepted if already Live/PartiallyFilled/Terminal.
    if (ord.state != ManagedOrderState::PendingNew &&
        ord.state != ManagedOrderState::Uncertain) {
      ++summary_.duplicate_reports_ignored;
      return false;
    }
    ord.exchange_order_id = report.exchange_order_id;
    ord.state = (ord.cum_filled_qty.lots > 0) ? ManagedOrderState::PartiallyFilled
                                              : ManagedOrderState::Live;
    ord.last_update_ns = now_ns;
    ++summary_.accepted;
    if (was_uncertain) ++summary_.uncertain_reconciled;
    if (journal_ != nullptr) {
      journal_->record_exec_report(
          instrument, report, static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
          journal_seq_++);
    }
    return true;
  }

  if (report.type == ExecReportType::Canceled) {
    if (is_terminal_order_state(ord.state)) {
      ++summary_.duplicate_reports_ignored;
      return false;
    }
    const std::uint64_t target_leaves = std::min(report.leaves_qty.lots, ord.leaves_qty.lots);
    const std::uint64_t delta_canceled = ord.leaves_qty.lots - target_leaves;
    ord.canceled_qty.lots += delta_canceled;
    ord.leaves_qty.lots = target_leaves;
    ord.state = (ord.leaves_qty.lots == 0)
                    ? ((ord.cum_filled_qty.lots == ord.submitted_qty.lots)
                           ? ManagedOrderState::Filled
                           : ManagedOrderState::Canceled)
                    : ManagedOrderState::Live;
    ord.cancel_requested = false;
    ord.last_update_ns = now_ns;
    ++summary_.canceled;
    if (was_uncertain) ++summary_.uncertain_reconciled;
    if (journal_ != nullptr) {
      journal_->record_exec_report(
          instrument, report, static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
          journal_seq_++);
    }
    return true;
  }

  if (report.type == ExecReportType::Rejected) {
    if (is_terminal_order_state(ord.state)) {
      ++summary_.duplicate_reports_ignored;
      return false;
    }
    ord.canceled_qty.lots = ord.submitted_qty.lots - ord.cum_filled_qty.lots;
    ord.leaves_qty.lots = 0;
    ord.state = ManagedOrderState::Rejected;
    ord.last_update_ns = now_ns;
    ++summary_.rejected;
    if (was_uncertain) ++summary_.uncertain_reconciled;
    if (journal_ != nullptr) {
      journal_->record_exec_report(
          instrument, report, static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)),
          journal_seq_++);
    }
    return true;
  }

  if (report.type == ExecReportType::CancelRejected) {
    // Common in cancel/fill race: order filled before cancel reached the matching engine.
    // Never mutate a Filled order back to Canceled!
    ord.cancel_requested = false;
    if (ord.state == ManagedOrderState::PendingCancel) {
      ord.state = (ord.leaves_qty.lots == 0)
                      ? ManagedOrderState::Filled
                      : (ord.cum_filled_qty.lots > 0 ? ManagedOrderState::PartiallyFilled
                                                     : ManagedOrderState::Live);
    }
    ord.last_update_ns = now_ns;
    ++summary_.cancel_rejected;
    return true;
  }

  return false;
}

std::size_t OmsReconciler::on_session_disconnect(VenueId venue, SimTimeNs now_ns) {
  std::size_t count = 0;
  for (auto& [tok, ord] : orders_) {
    if (ord.venue == venue && !is_terminal_order_state(ord.state)) {
      ord.state = ManagedOrderState::Uncertain;
      ord.last_update_ns = now_ns;
      ++summary_.uncertain_marked;
      ++count;
    }
  }
  if (journal_ != nullptr) {
    journal_->record_session_down(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)));
  }
  return count;
}

std::size_t OmsReconciler::reconcile_on_reconnect(
    VenueId venue,
    const std::vector<std::pair<InstrumentId, ExecutionReport>>& missed_reports,
    SimTimeNs now_ns) {
  if (journal_ != nullptr) {
    journal_->record_session_up(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ns.value)), journal_seq_++);
  }
  std::size_t applied = 0;
  for (const auto& [inst, rep] : missed_reports) {
    if (on_execution_report(venue, inst, rep, now_ns)) {
      ++applied;
    }
  }
  return applied;
}

ExposureSnapshot OmsReconciler::exposure(InstrumentId instrument) const {
  ExposureSnapshot exp{};
  exp.confirmed_position = ledger_.net_position(instrument);
  for (const auto& [tok, ord] : orders_) {
    if (ord.instrument != instrument) continue;
    if (is_terminal_order_state(ord.state)) continue;
    // Includes PendingNew, Live, PartiallyFilled, PendingCancel, AND Uncertain!
    exp.open_order_count += 1;
    if (ord.side == Side::Buy) {
      exp.pending_buy_lots += ord.leaves_qty.lots;
    } else {
      exp.pending_sell_lots += ord.leaves_qty.lots;
    }
  }
  return exp;
}

const ManagedOrder* OmsReconciler::find(const ClientToken& tok) const {
  const auto it = orders_.find(tok);
  return it == orders_.end() ? nullptr : &it->second;
}

std::vector<const ManagedOrder*> OmsReconciler::uncertain_orders() const {
  std::vector<const ManagedOrder*> out;
  for (const auto& [tok, ord] : orders_) {
    if (ord.state == ManagedOrderState::Uncertain) out.push_back(&ord);
  }
  return out;
}

std::string OmsReconciler::check_invariants() const {
  for (const auto& [tok, ord] : orders_) {
    if (ord.cum_filled_qty.lots > ord.submitted_qty.lots) {
      return "cum_filled > submitted_qty for token " + tok.to_string();
    }
    const std::uint64_t accounted =
        ord.cum_filled_qty.lots + ord.leaves_qty.lots + ord.canceled_qty.lots;
    if (accounted != ord.submitted_qty.lots) {
      return "qty conservation violated (filled+leaves+canceled != submitted) for token " +
             tok.to_string();
    }
    if (is_terminal_order_state(ord.state) && ord.leaves_qty.lots != 0) {
      return "terminal order has non-zero leaves for token " + tok.to_string();
    }
    if (!is_terminal_order_state(ord.state) && ord.leaves_qty.lots == 0) {
      return "non-terminal order has zero leaves for token " + tok.to_string();
    }
  }
  return {};
}

}  // namespace hft::runtime
