#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/scanner.hpp"
#include "xarb/types.hpp"

namespace xarb {

enum class ExecutionOutcome : uint8_t {
  Pending = 0,
  BothFilledProfitable = 1,
  BothFilledLossSlippage = 2,
  OneLeggedUnhedged = 3,
  BothMissedExpired = 4,
  RejectedUnfunded = 5,
};

inline std::string_view execution_outcome_name(ExecutionOutcome o) noexcept {
  switch (o) {
    case ExecutionOutcome::Pending: return "pending";
    case ExecutionOutcome::BothFilledProfitable: return "both_filled_profitable";
    case ExecutionOutcome::BothFilledLossSlippage: return "both_filled_loss_slippage";
    case ExecutionOutcome::OneLeggedUnhedged: return "one_legged_unhedged";
    case ExecutionOutcome::BothMissedExpired: return "both_missed_expired";
    case ExecutionOutcome::RejectedUnfunded: return "rejected_unfunded";
  }
  return "unknown";
}

struct VenueBalance {
  double quote_available{0.0};
  double quote_reserved{0.0};
  double base_available{0.0};
  double base_reserved{0.0};
  double total_fees_paid_quote{0.0};
  double cumulative_bought_base{0.0};
  double cumulative_sold_base{0.0};
};

struct PaperConfig {
  std::array<double, kVenueCount> initial_quote_per_venue{50000.0, 50000.0, 50000.0};
  std::array<double, kVenueCount> initial_base_per_venue{1.0, 1.0, 1.0};
  // Order submission transit latency per venue (ns).
  std::array<TimestampNs, kVenueCount> order_latency_ns{5LL * kNsPerMs, 8LL * kNsPerMs, 6LL * kNsPerMs};
  // Symmetric uniform jitter +/- order_jitter_ns (seeded deterministically).
  TimestampNs order_jitter_ns{1LL * kNsPerMs};
  // Limit-IOC (true) uses signal's worst-level price as limit (models miss/one-leg risk);
  // Market-IOC (false) walks full book (models slippage risk).
  bool use_limit_ioc{true};
  // Extra price tolerance in bps on Limit-IOC orders.
  double limit_slippage_tolerance_bps{0.0};
  // Optional non-instantaneous rebalance simulation when a venue's base drops below threshold.
  bool enable_rebalance_model{false};
  double rebalance_min_base_ratio{0.20};
};

struct PendingLegOrder {
  uint64_t trade_id{0};
  uint32_t instrument_id{0};
  VenueId venue{VenueId::Binance};
  bool is_buy{true};
  TimestampNs submit_ns{0};
  TimestampNs arrival_ns{0};
  QtyLots target_lots{0};
  PriceTicks limit_ticks{0};  // 0 = market IOC
  double reserved_amount{0.0};  // quote reserved (buy) or base reserved (sell)
};

struct RebalanceTransfer {
  uint64_t transfer_id{0};
  uint32_t instrument_id{0};
  VenueId from_venue{VenueId::Binance};
  VenueId to_venue{VenueId::Coinbase};
  double amount_base{0.0};
  double fee_base{0.0};
  TimestampNs start_ns{0};
  TimestampNs arrival_ns{0};
  bool completed{false};
};

struct PaperTradeRecord {
  uint64_t trade_id{0};
  uint64_t signal_id{0};
  uint32_t instrument_id{0};
  VenueId buy_venue{VenueId::Binance};
  VenueId sell_venue{VenueId::Coinbase};
  TimestampNs detect_ns{0};
  TimestampNs buy_arrival_ns{0};
  TimestampNs sell_arrival_ns{0};

  QtyLots target_lots{0};
  double expected_buy_vwap{0.0};
  double expected_sell_vwap{0.0};
  double expected_net_profit_quote{0.0};
  double expected_net_edge_bps{0.0};
  double gross_spread_bps{0.0};

  bool buy_done{false};
  bool sell_done{false};
  QtyLots buy_filled_lots{0};
  QtyLots sell_filled_lots{0};
  double buy_exec_vwap{0.0};
  double sell_exec_vwap{0.0};
  double buy_fee_quote{0.0};
  double sell_fee_quote{0.0};

  // Matched hedged P&L + mark-to-market of any unhedged residual at completion time.
  double realized_matched_pnl_quote{0.0};
  double unhedged_base_delta{0.0};
  double unhedged_mtm_pnl_quote{0.0};
  double total_pnl_quote{0.0};
  double slippage_vs_expected_quote{0.0};
  ExecutionOutcome outcome{ExecutionOutcome::Pending};
};

struct PaperStats {
  uint64_t trades_attempted{0};
  uint64_t trades_completed{0};
  uint64_t both_filled_profitable{0};
  uint64_t both_filled_loss_slippage{0};
  uint64_t one_legged_unhedged{0};
  uint64_t both_missed_expired{0};
  uint64_t rejected_unfunded{0};
  uint64_t rebalances_initiated{0};
  uint64_t rebalances_completed{0};
  double total_rebalance_fees_quote{0.0};
  double total_turnover_quote{0.0};
  double total_fees_quote{0.0};
  double total_realized_matched_pnl_quote{0.0};
  double total_unhedged_mtm_pnl_quote{0.0};
  double net_simulated_pnl_quote{0.0};
  double max_drawdown_quote{0.0};
};

class PaperExecutor {
 public:
  PaperExecutor(const InstrumentRegistry& registry, PaperConfig cfg);

  const PaperConfig& config() const noexcept { return cfg_; }
  const PaperStats& stats() const noexcept { return stats_; }
  const std::vector<PaperTradeRecord>& trades() const noexcept { return trades_; }
  const VenueBalance& balance(uint32_t inst_id, VenueId v) const;
  RouteFundingLimits funding_for_route(uint32_t inst_id, VenueId buy_v, VenueId sell_v) const;

  // Attempts to reserve pre-funded balances and create two delayed IOC paper legs.
  // Returns legs to be scheduled by the event loop (empty if rejected for insufficient funding).
  std::vector<PendingLegOrder> submit_signal(const ArbitrageSignal& sig, TimestampNs buy_arrival_ns,
                                             TimestampNs sell_arrival_ns);

  // Executes an arriving leg against the ACTUAL live/truth order book at arrival time.
  // Returns true if this leg finalized the parent trade.
  bool execute_leg(const PendingLegOrder& leg, const FlatOrderBook& truth_book, double mark_mid_price_quote);

  // Optionally initiates a non-instantaneous base rebalance transfer if a venue is depleted.
  std::optional<RebalanceTransfer> maybe_initiate_rebalance(uint32_t inst_id, TimestampNs now_ns);
  void complete_rebalance(const RebalanceTransfer& xfer, double mark_mid_price_quote);

  // Computes total portfolio equity (total quote + total base * mark_price) across all venues.
  double total_equity_quote(uint32_t inst_id, double mark_mid_price_quote) const;
  double initial_equity_quote(uint32_t inst_id, double mark_mid_price_quote) const;

  // Accounting conservation check:
  // No negative available/reserved balances, and total base + quote changes match exact fill/fee/rebalance sums.
  bool verify_invariants(uint32_t inst_id, double tolerance = 1e-6) const;

 private:
  void finalize_trade(PaperTradeRecord& tr, double mark_mid_price_quote);

  const InstrumentRegistry& registry_;
  PaperConfig cfg_;
  // balances_[inst_id][venue]
  std::vector<std::array<VenueBalance, kVenueCount>> balances_;
  std::vector<double> initial_total_quote_;
  std::vector<double> initial_total_base_;
  std::vector<double> cumulative_base_rebalance_fees_;
  std::vector<double> in_flight_rebalance_base_;

  std::vector<PaperTradeRecord> trades_;
  std::unordered_map<uint64_t, size_t> trade_index_;
  PaperStats stats_{};
  uint64_t next_trade_id_{1};
  uint64_t next_transfer_id_{1};
  double peak_pnl_{0.0};
};

}  // namespace xarb
