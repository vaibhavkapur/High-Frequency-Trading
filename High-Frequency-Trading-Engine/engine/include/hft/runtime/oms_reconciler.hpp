#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hft/adapters/ouch_adapter.hpp"
#include "hft/contracts/v1/events.hpp"
#include "hft/runtime/ledger.hpp"
#include "hft/runtime/risk_gate.hpp"

namespace hft::runtime {

enum class ManagedOrderState : std::uint8_t {
  PendingNew = 0,
  Live = 1,
  PartiallyFilled = 2,
  PendingCancel = 3,
  Uncertain = 4,
  Filled = 5,
  Canceled = 6,
  Rejected = 7,
  Replaced = 8,
};

constexpr bool is_terminal_order_state(ManagedOrderState s) noexcept {
  return s == ManagedOrderState::Filled || s == ManagedOrderState::Canceled ||
         s == ManagedOrderState::Rejected || s == ManagedOrderState::Replaced;
}

struct ManagedOrder {
  contracts::v1::VenueId venue = 0;
  contracts::v1::InstrumentId instrument = 0;
  contracts::v1::OrderId client_order_id = 0;
  contracts::v1::ClientToken token{};
  contracts::v1::ExchangeOrderId exchange_order_id = 0;
  contracts::v1::Side side = contracts::v1::Side::Buy;
  contracts::v1::Price price{0};
  contracts::v1::Quantity submitted_qty{0};
  contracts::v1::Quantity cum_filled_qty{0};
  contracts::v1::Quantity leaves_qty{0};
  contracts::v1::Quantity canceled_qty{0};
  ManagedOrderState state = ManagedOrderState::PendingNew;
  bool cancel_requested = false;
  contracts::v1::SimTimeNs submit_time_ns{0};
  contracts::v1::SimTimeNs last_update_ns{0};
};

struct OmsSummary {
  std::uint64_t submitted = 0;
  std::uint64_t accepted = 0;
  std::uint64_t partial_fills = 0;
  std::uint64_t filled = 0;
  std::uint64_t canceled = 0;
  std::uint64_t rejected = 0;
  std::uint64_t cancel_rejected = 0;
  std::uint64_t duplicate_reports_ignored = 0;
  std::uint64_t uncertain_marked = 0;
  std::uint64_t uncertain_reconciled = 0;
};

// Authoritative OMS & recovery state machine (ADR-003).
// - Enforces: cum_filled <= submitted_qty, leaves == submitted - cum_filled - canceled,
//   unacknowledged & uncertain orders remain in worst-case exposure, lost cancel responses are
//   never treated as cancellations, and reconnect reconciles by token rather than minting fresh tokens.
// - Optionally mirrors all wire requests/responses into Project 03's OuchJournalBridge binary log.
class OmsReconciler {
 public:
  explicit OmsReconciler(AccountingLedger& ledger,
                         adapters::OuchJournalBridge* journal = nullptr);

  // Register outbound order intent (after PreTradeRiskGate approval).
  bool on_order_sent(contracts::v1::VenueId venue, contracts::v1::InstrumentId instrument,
                     const contracts::v1::OrderIntent& intent, contracts::v1::SimTimeNs now_ns);

  // Apply inbound execution report (idempotent on ExecId and terminal transitions).
  // Returns true if report produced a new state/ledger update, false if duplicate/ignored.
  bool on_execution_report(contracts::v1::VenueId venue, contracts::v1::InstrumentId instrument,
                           const contracts::v1::ExecutionReport& report,
                           contracts::v1::SimTimeNs now_ns);

  // Mark all non-terminal orders on `venue` as Uncertain upon gateway disconnect.
  // Crucially, uncertain orders STAY counted in ExposureSnapshot so risk never drops them.
  std::size_t on_session_disconnect(contracts::v1::VenueId venue,
                                    contracts::v1::SimTimeNs now_ns);

  // Reconcile uncertain orders against authoritative sequenced exchange truth reports on reconnect
  // WITHOUT blindly resending under new tokens.
  std::size_t reconcile_on_reconnect(
      contracts::v1::VenueId venue,
      const std::vector<std::pair<contracts::v1::InstrumentId, contracts::v1::ExecutionReport>>&
          missed_reports,
      contracts::v1::SimTimeNs now_ns);

  [[nodiscard]] ExposureSnapshot exposure(contracts::v1::InstrumentId instrument) const;
  [[nodiscard]] const ManagedOrder* find(const contracts::v1::ClientToken& tok) const;
  [[nodiscard]] std::vector<const ManagedOrder*> uncertain_orders() const;
  [[nodiscard]] const OmsSummary& summary() const noexcept { return summary_; }
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

  // Full order-level conservation invariant check (empty string = all invariants hold).
  [[nodiscard]] std::string check_invariants() const;

 private:
  static std::uint64_t make_exec_key(contracts::v1::VenueId venue,
                                     contracts::v1::ExecId exec_id) noexcept {
    return (static_cast<std::uint64_t>(venue) << 48U) ^ exec_id;
  }

  AccountingLedger& ledger_;
  adapters::OuchJournalBridge* journal_;
  std::unordered_map<contracts::v1::ClientToken, ManagedOrder, contracts::v1::ClientTokenHash>
      orders_;
  std::unordered_set<std::uint64_t> seen_exec_ids_;
  std::uint64_t journal_seq_ = 1;
  OmsSummary summary_{};
};

}  // namespace hft::runtime
