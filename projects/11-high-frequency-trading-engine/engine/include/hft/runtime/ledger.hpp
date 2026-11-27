#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hft/contracts/v1/events.hpp"

namespace hft::runtime {

struct ExecutedFillRecord {
  contracts::v1::VenueId venue = 0;
  contracts::v1::InstrumentId instrument = 0;
  contracts::v1::ExecId exec_id = 0;
  contracts::v1::ClientToken token{};
  contracts::v1::Side side = contracts::v1::Side::Buy;
  contracts::v1::Price price{0};
  contracts::v1::Quantity qty{0};
  contracts::v1::Money notional_scaled{0};
  contracts::v1::Money fee_scaled{0};
  contracts::v1::Money cash_delta_scaled{0};
  contracts::v1::SimTimeNs time_ns{0};
};

struct InstrumentLedgerState {
  std::int64_t position_lots = 0;
  contracts::v1::Money cash_scaled{0};
  contracts::v1::Money cost_basis_scaled{0};
  contracts::v1::Money realized_pnl_scaled{0};
  contracts::v1::Money fees_scaled{0};
  contracts::v1::Price last_mark_price{0};
  std::uint64_t traded_lots = 0;
  std::uint64_t fill_count = 0;
};

struct ReconciliationReport {
  bool ok = true;
  contracts::v1::Money initial_cash{0};
  contracts::v1::Money cash_scaled{0};
  contracts::v1::Money mark_inventory_value{0};
  contracts::v1::Money total_fees{0};
  contracts::v1::Money realized_pnl{0};
  contracts::v1::Money unrealized_pnl{0};
  contracts::v1::Money net_equity_cash_plus_inv{0};
  contracts::v1::Money net_equity_pnl_decomp{0};
  std::int64_t discrepancy_scaled = 0;
  std::uint64_t unique_fills = 0;
  std::uint64_t duplicate_exec_ids_ignored = 0;
  std::string failure_reason;
};

// Authoritative fixed-point integer accounting ledger (ADR-003).
// No floating point in accounting hot paths; deduplicates every fill by (VenueId, ExecId)
// and verifies exact conservation between execution journal, cash, marked inventory, fees, and P&L.
class AccountingLedger {
 public:
  AccountingLedger(std::vector<contracts::v1::InstrumentMeta> instruments,
                   contracts::v1::Money initial_cash = contracts::v1::Money{
                       1'000'000LL * contracts::v1::kMoneyScale});

  // Returns true if fill was applied, false if duplicate (VenueId, ExecId) was ignored.
  bool on_fill(contracts::v1::VenueId venue, contracts::v1::InstrumentId instrument,
               const contracts::v1::ExecutionReport& report);

  void update_mark(contracts::v1::InstrumentId instrument, contracts::v1::Price mark_price);

  [[nodiscard]] std::int64_t net_position(contracts::v1::InstrumentId instrument) const noexcept;
  [[nodiscard]] std::int64_t venue_position(contracts::v1::VenueId venue,
                                            contracts::v1::InstrumentId instrument) const noexcept;
  [[nodiscard]] const InstrumentLedgerState& instrument_state(
      contracts::v1::InstrumentId instrument) const;
  [[nodiscard]] const std::vector<ExecutedFillRecord>& execution_history() const noexcept {
    return fills_;
  }
  [[nodiscard]] std::uint64_t duplicate_exec_ids_ignored() const noexcept {
    return duplicate_exec_ids_ignored_;
  }

  // Independent full cross-check: recomputes cash, position, and fees from raw execution_history_
  // and compares against live ledger balances and P&L decomposition.
  [[nodiscard]] ReconciliationReport reconcile() const;

 private:
  static std::uint64_t make_exec_key(contracts::v1::VenueId venue,
                                     contracts::v1::ExecId exec_id) noexcept {
    return (static_cast<std::uint64_t>(venue) << 48U) ^ exec_id;
  }

  static std::uint64_t make_vi_key(contracts::v1::VenueId venue,
                                   contracts::v1::InstrumentId inst) noexcept {
    return (static_cast<std::uint64_t>(venue) << 32U) | inst;
  }

  std::vector<contracts::v1::InstrumentMeta> instruments_;
  contracts::v1::Money initial_cash_{0};
  contracts::v1::Money total_cash_{0};
  std::vector<InstrumentLedgerState> by_instrument_;
  std::unordered_map<std::uint64_t, std::int64_t> venue_positions_;
  std::unordered_set<std::uint64_t> seen_exec_keys_;
  std::vector<ExecutedFillRecord> fills_;
  std::uint64_t duplicate_exec_ids_ignored_ = 0;
};

}  // namespace hft::runtime
