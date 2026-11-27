#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"
#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/paper_executor.hpp"
#include "xarb/scanner.hpp"

namespace hft::adapters {

// Paper-only cross-exchange arbitrage adapter wrapping Project 10 (xarb::InstrumentRegistry,
// xarb::FlatOrderBook, xarb::compute_executable_cross, xarb::PaperExecutor).
// Explicitly models fees, depth walk, venue funding limits, latency decay, quote staleness,
// and partial/one-legged execution risk — including negative cases where positive gross
// spreads fail net execution feasibility.
class PaperArbitrageAdapter {
 public:
  struct Config {
    double min_net_edge_bps = 3.0;
    double slippage_buffer_bps = 1.0;
    double min_net_profit_quote = 0.50;
    std::int64_t max_stale_ns = 50'000'000;  // 50 ms
    std::array<double, xarb::kVenueCount> initial_quote_per_venue{50'000.0, 50'000.0, 50'000.0};
    std::array<double, xarb::kVenueCount> initial_base_per_venue{500.0, 500.0, 500.0};
    std::array<std::int64_t, xarb::kVenueCount> leg_latency_ns{2'000'000, 5'000'000, 3'000'000};
    bool use_limit_ioc = true;
  };

  struct VenueSnapshotInput {
    contracts::v1::VenueId venue = 0;
    contracts::v1::SimTimeNs last_update_ns{0};
    bool synced = true;
    std::vector<contracts::v1::LevelView> bids;
    std::vector<contracts::v1::LevelView> asks;
  };

  struct EvaluationOutcome {
    contracts::v1::SimTimeNs eval_time_ns{0};
    contracts::v1::VenueId buy_venue = 0;
    contracts::v1::VenueId sell_venue = 0;
    double gross_spread_bps = 0.0;
    double net_edge_bps = 0.0;
    double estimated_fees_quote = 0.0;
    double estimated_net_profit_quote = 0.0;
    std::uint64_t executable_lots = 0;
    xarb::SignalRejectionReason status = xarb::SignalRejectionReason::NoGrossCross;
    bool gross_positive_net_rejected = false;
  };

  struct PaperSummary {
    std::uint64_t scans = 0;
    std::uint64_t gross_crosses = 0;
    std::uint64_t actionable_signals = 0;
    std::uint64_t gross_positive_net_infeasible = 0;
    std::uint64_t rejected_fees = 0;
    std::uint64_t rejected_stale = 0;
    std::uint64_t rejected_depth = 0;
    std::uint64_t rejected_funding = 0;
    xarb::PaperStats paper_stats{};
    bool invariants_ok = false;
    double initial_equity_quote = 0.0;
    double final_equity_quote = 0.0;
  };

  PaperArbitrageAdapter(const contracts::v1::InstrumentMeta& instrument,
                        const std::vector<contracts::v1::VenueMeta>& venues)
      : PaperArbitrageAdapter(instrument, venues, Config{}) {}
  PaperArbitrageAdapter(const contracts::v1::InstrumentMeta& instrument,
                        const std::vector<contracts::v1::VenueMeta>& venues, Config cfg);

  // Evaluate normalized multi-venue books at eval_time_ns (observed view).
  // Returns all pairwise cross evaluations (including negative gross-vs-net cases).
  std::vector<EvaluationOutcome> evaluate_observed_snapshots(
      contracts::v1::SimTimeNs eval_time_ns,
      const std::vector<VenueSnapshotInput>& observed_snapshots);

  // Execute an actionable opportunity against delayed future truth books (models latency decay,
  // partial fills, and one-legged unhedged execution risk through Project 10's PaperExecutor).
  xarb::ExecutionOutcome execute_paper_against_delayed_truth(
      const EvaluationOutcome& opp,
      const std::vector<VenueSnapshotInput>& observed_at_detect,
      const std::vector<VenueSnapshotInput>& truth_at_buy_arrival,
      const std::vector<VenueSnapshotInput>& truth_at_sell_arrival,
      double mark_mid_quote);

  [[nodiscard]] PaperSummary summary(double mark_mid_quote) const;
  [[nodiscard]] const xarb::PaperExecutor& paper_executor() const noexcept { return *executor_; }

 private:
  static xarb::FlatOrderBook to_xarb_book(const VenueSnapshotInput& snap);
  static xarb::VenueId to_xarb_venue(contracts::v1::VenueId v) noexcept;

  contracts::v1::InstrumentMeta instrument_;
  Config cfg_;
  xarb::InstrumentRegistry registry_;
  std::unique_ptr<xarb::PaperExecutor> executor_;
  std::uint64_t next_signal_id_ = 1;
  PaperSummary stats_{};
};

}  // namespace hft::adapters
