#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hft/adapters/arb_adapter.hpp"
#include "hft/adapters/backtester_adapter.hpp"
#include "hft/adapters/itch_adapter.hpp"
#include "hft/adapters/matching_adapter.hpp"
#include "hft/adapters/multicast_adapter.hpp"
#include "hft/adapters/ouch_adapter.hpp"
#include "hft/adapters/perf_adapter.hpp"
#include "hft/adapters/router_adapter.hpp"
#include "hft/adapters/spsc_adapter.hpp"
#include "hft/adapters/strategy_adapter.hpp"
#include "hft/contracts/v1/interfaces.hpp"
#include "hft/runtime/canonical_book.hpp"
#include "hft/runtime/fault_injector.hpp"
#include "hft/runtime/ledger.hpp"
#include "hft/runtime/oms_reconciler.hpp"
#include "hft/runtime/risk_gate.hpp"
#include "hft/runtime/scheduler.hpp"

namespace hft::runtime {

enum class RuntimeMode : std::uint8_t {
  Deterministic = 0,
  NetworkLab = 1,
  Performance = 2,
};

struct ScenarioConfig {
  std::string scenario_name = "single-venue";
  RuntimeMode mode = RuntimeMode::Deterministic;
  adapters::BookBackend book_backend = adapters::BookBackend::Pool;
  std::uint64_t seed = 42;
  std::size_t event_count = 200;
  std::string journal_path;
  RiskConfig risk{};
  ExecFaultConfig exec_faults{};
  bool use_sor = false;
  sor::RoutingPolicy sor_policy = sor::RoutingPolicy::SmartCostAware;
};

struct DeterministicRunResult {
  std::string scenario_name;
  std::string book_backend_name;
  std::uint64_t seed = 0;
  std::uint64_t events_scheduled = 0;
  std::uint64_t md_events_observed = 0;
  std::uint64_t intents_emitted = 0;
  std::uint64_t risk_approved = 0;
  std::uint64_t risk_rejected = 0;
  std::uint64_t orders_accepted = 0;
  std::uint64_t fills_executed = 0;
  std::uint64_t orders_canceled = 0;
  std::uint64_t orders_rejected = 0;
  std::uint64_t duplicates_ignored = 0;
  std::uint64_t causality_violations = 0;
  std::uint64_t replay_checksum = 0;
  std::uint64_t final_book_hash = 0;
  std::int64_t final_position_lots = 0;
  contracts::v1::Money cash_scaled{0};
  contracts::v1::Money realized_pnl_scaled{0};
  contracts::v1::Money unrealized_pnl_scaled{0};
  contracts::v1::Money fees_scaled{0};
  contracts::v1::Money net_equity_scaled{0};
  std::int64_t ledger_discrepancy_scaled = 0;
  bool exchange_invariants_ok = false;
  bool oms_invariants_ok = false;
  bool ledger_reconciled_ok = false;
  std::string invariant_error;
  std::vector<std::string> trace_sample;

  [[nodiscard]] bool all_invariants_ok() const noexcept {
    return causality_violations == 0 && exchange_invariants_ok && oms_invariants_ok &&
           ledger_reconciled_ok && ledger_discrepancy_scaled == 0;
  }

  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::string to_text_summary() const;
};

// Executes the deterministic end-to-end pipeline over canonical fixtures or synthetic feeds:
//   Feed -> Venue Truth Book (Project 01) -> Delayed Observation (Project 04 LatencyPath) ->
//   Observed Canonical Book -> Strategy Plugin -> Pre-Trade Risk -> (Optional SOR Project 07) ->
//   Gateway/OMS/Journal (Project 03) -> Matching Engine -> Execution Reports -> Accounting Ledger.
class DeterministicEnginePipeline {
 public:
  DeterministicRunResult run_single_venue(const ScenarioConfig& cfg,
                                          const std::vector<contracts::v1::Envelope>& feed,
                                          contracts::v1::IStrategy& strategy,
                                          const std::vector<contracts::v1::InstrumentMeta>& insts,
                                          const contracts::v1::VenueMeta& venue);

  DeterministicRunResult run_multi_venue(const ScenarioConfig& cfg,
                                         const std::vector<contracts::v1::Envelope>& feed,
                                         contracts::v1::IStrategy& strategy,
                                         const std::vector<contracts::v1::InstrumentMeta>& insts,
                                         const std::vector<contracts::v1::VenueMeta>& venues);

  // Generates the deterministic single-venue L3 fixture used for Phase 2 golden verification.
  static std::vector<contracts::v1::Envelope> make_golden_single_venue_feed(
      contracts::v1::VenueId venue = 0, contracts::v1::InstrumentId instrument = 0);

  // Generates a deterministic multi-venue L3 feed across 3 venues with independent depth & ticks.
  static std::vector<contracts::v1::Envelope> make_multi_venue_feed(
      std::size_t steps, std::uint64_t seed, std::size_t num_venues = 3,
      std::size_t num_instruments = 2);
};

// Stage-level and end-to-end wall-clock performance suite (Phase 6, Performance mode).
struct PerfSuiteResult {
  lpl::EnvironmentInfo env{};
  std::vector<lpl::BenchmarkResult> stage_benchmarks;
  std::vector<lpl::BenchmarkResult> e2e_benchmarks;
  lpl::DistributionStats tick_to_decision_ns{};
  lpl::DistributionStats order_submit_to_ack_ns{};
  lpl::DistributionStats order_submit_to_fill_ns{};
  bool pool_vs_map_checksum_match = false;
  std::uint64_t pool_checksum = 0;
  std::uint64_t map_checksum = 0;
  std::uint64_t direct_checksum = 0;
  std::uint64_t spsc_checksum = 0;
  std::uint64_t spsc_drops = 0;
};

PerfSuiteResult run_performance_suite(std::size_t events_per_sample = 500,
                                      std::size_t repetitions = 5,
                                      std::size_t samples_per_rep = 20);

}  // namespace hft::runtime
