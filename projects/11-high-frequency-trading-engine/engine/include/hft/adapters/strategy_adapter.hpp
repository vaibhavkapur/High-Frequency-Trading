#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"
#include "mmsim/config.hpp"
#include "mmsim/features.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/strategy.hpp"

namespace hft::adapters {

// Deterministic single-venue strategy used in Phase 2 MVP (`single-venue` scenario).
// Exercises passive limit placement, aggressive partial/full fills, cancels, and risk rejects
// strictly from delayed observed market data (never future truth).
class SimpleDeterministicStrategy final : public contracts::v1::IStrategy {
 public:
  struct Config {
    contracts::v1::Quantity quote_qty{20};
    std::int64_t max_position = 100;
    std::uint64_t act_every_n_events = 3;
    bool inject_oversize_test_order = true;
  };

  SimpleDeterministicStrategy() : SimpleDeterministicStrategy(Config{}) {}
  explicit SimpleDeterministicStrategy(Config cfg);

  [[nodiscard]] std::string name() const override { return "simple_deterministic"; }

  void on_observed_market_data(const contracts::v1::IObservedMarketView& view,
                               contracts::v1::VenueId venue,
                               contracts::v1::InstrumentId instrument,
                               const contracts::v1::MarketDataEvent& md,
                               std::vector<contracts::v1::OrderIntent>& out_intents) override;

  void on_execution_report(const contracts::v1::IObservedMarketView& view,
                           contracts::v1::VenueId venue,
                           contracts::v1::InstrumentId instrument,
                           const contracts::v1::ExecutionReport& report,
                           std::vector<contracts::v1::OrderIntent>& out_intents) override;

 private:
  Config cfg_;
  std::uint64_t seen_events_ = 0;
  contracts::v1::OrderId next_client_id_ = 1;
  contracts::v1::ClientToken active_bid_token_{};
  bool has_active_bid_ = false;
  bool sent_oversize_probe_ = false;
  bool sent_crossing_order_ = false;
};

// Strategy plugin wrapping Project 08 (mmsim::FeatureEngine + mmsim::InventoryAwareStrategy /
// mmsim::NaiveSymmetricStrategy + observed-only mmsim::FlatArrayBook).
// Preserves strict observed-vs-truth separation: feeds only delayed observed events into
// Project 08's feature engine and quoting policy.
class MarketMakerPlugin final : public contracts::v1::IStrategy {
 public:
  struct Config {
    mmsim::StrategyConfig strategy{};
    mmsim::FeatureConfig features{};
    std::int64_t max_abs_position = 200;
    double maker_fee_bps = -0.2;
    std::string token_prefix = "MM";
  };

  MarketMakerPlugin() : MarketMakerPlugin(Config{}) {}
  explicit MarketMakerPlugin(Config cfg);

  [[nodiscard]] std::string name() const override;

  void on_observed_market_data(const contracts::v1::IObservedMarketView& view,
                               contracts::v1::VenueId venue,
                               contracts::v1::InstrumentId instrument,
                               const contracts::v1::MarketDataEvent& md,
                               std::vector<contracts::v1::OrderIntent>& out_intents) override;

  void on_execution_report(const contracts::v1::IObservedMarketView& view,
                           contracts::v1::VenueId venue,
                           contracts::v1::InstrumentId instrument,
                           const contracts::v1::ExecutionReport& report,
                           std::vector<contracts::v1::OrderIntent>& out_intents) override;

  [[nodiscard]] const mmsim::QuoteDecision& last_decision() const noexcept {
    return last_decision_;
  }
  [[nodiscard]] const mmsim::BookSignals& last_signals() const noexcept { return last_signals_; }
  [[nodiscard]] std::uint64_t quote_decisions() const noexcept { return quote_decisions_; }

 private:
  struct ActiveQuote {
    bool live = false;
    contracts::v1::OrderId client_order_id = 0;
    contracts::v1::ClientToken token{};
    contracts::v1::Price price{0};
    contracts::v1::Quantity qty{0};
    contracts::v1::SimTimeNs placed_ns{0};
  };

  struct InstrumentContext {
    std::unique_ptr<mmsim::IL2Book> observed_book;
    std::unique_ptr<mmsim::FeatureEngine> features;
    ActiveQuote bid{};
    ActiveQuote ask{};
    std::uint64_t md_seq = 0;
  };

  InstrumentContext& get_context(contracts::v1::VenueId venue,
                                 contracts::v1::InstrumentId instrument);

  Config cfg_;
  std::unique_ptr<mmsim::IStrategy> impl_;
  std::unordered_map<std::uint64_t, InstrumentContext> contexts_;
  contracts::v1::OrderId next_client_id_ = 10'000;
  mmsim::BookSignals last_signals_{};
  mmsim::QuoteDecision last_decision_{};
  std::uint64_t quote_decisions_ = 0;
};

}  // namespace hft::adapters
