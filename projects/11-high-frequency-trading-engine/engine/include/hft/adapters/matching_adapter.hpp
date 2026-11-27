#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"
#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"
#include "ouch/sim/matching_engine.hpp"

namespace hft::adapters {

enum class BookBackend : std::uint8_t {
  Pool = 0,
  Map = 1,
};

// Wraps Project 01 (me::Engine<me::PoolBook> or me::Engine<me::MapBook>) behind
// canonical contracts::v1::IExchange.
// Separates market-feed resting order IDs (bit 63 set) from strategy exchange order IDs
// (bit 63 clear) so passive strategy fills on incoming market flow are captured deterministically.
class MatchingEngineAdapter final : public contracts::v1::IExchange {
 public:
  MatchingEngineAdapter(contracts::v1::VenueMeta venue,
                        std::vector<contracts::v1::InstrumentMeta> instruments,
                        BookBackend backend = BookBackend::Pool,
                        std::uint32_t max_orders_per_book = 1U << 16U);

  [[nodiscard]] contracts::v1::VenueId venue_id() const noexcept override { return venue_.id; }
  [[nodiscard]] const contracts::v1::VenueMeta& venue_meta() const noexcept { return venue_; }
  [[nodiscard]] BookBackend backend() const noexcept { return backend_; }

  void apply_market_event(contracts::v1::InstrumentId instrument,
                          const contracts::v1::MarketDataEvent& md,
                          contracts::v1::SimTimeNs now_ns) override;

  // Same as apply_market_event, additionally collecting any passive fills triggered on
  // resting strategy orders when aggressive market flow crosses them.
  void apply_market_event_with_fills(contracts::v1::InstrumentId instrument,
                                     const contracts::v1::MarketDataEvent& md,
                                     contracts::v1::SimTimeNs now_ns,
                                     std::vector<contracts::v1::ExecutionReport>& out_passive_fills);

  contracts::v1::ErrorCode submit_order(
      contracts::v1::InstrumentId instrument, const contracts::v1::OrderIntent& intent,
      contracts::v1::SimTimeNs now_ns,
      std::vector<contracts::v1::ExecutionReport>& out_reports) override;

  [[nodiscard]] contracts::v1::TopOfBook top_of_book(
      contracts::v1::InstrumentId instrument) const override;
  [[nodiscard]] std::vector<contracts::v1::LevelView> depth(
      contracts::v1::InstrumentId instrument, contracts::v1::Side side,
      std::size_t max_levels) const override;
  [[nodiscard]] std::uint64_t book_hash(contracts::v1::InstrumentId instrument) const override;
  [[nodiscard]] bool check_invariants(std::string* why = nullptr) const override;

  [[nodiscard]] std::uint64_t total_commands() const noexcept;
  [[nodiscard]] std::uint64_t total_trades() const noexcept;
  [[nodiscard]] std::uint64_t next_exec_id() const noexcept { return next_exec_id_; }

 private:
  struct TrackedStrategyOrder {
    contracts::v1::InstrumentId instrument = 0;
    contracts::v1::OrderId client_order_id = 0;
    contracts::v1::ClientToken token{};
    contracts::v1::ExchangeOrderId exchange_id = 0;
    contracts::v1::Side side = contracts::v1::Side::Buy;
    contracts::v1::Price price{0};
    contracts::v1::Quantity requested_qty{0};
    contracts::v1::Quantity cum_filled{0};
    contracts::v1::Quantity leaves{0};
    bool live = false;
  };

  static constexpr std::uint64_t kFeedOrderMask = (1ULL << 63U);

  template <class Sink>
  void process_me_command(const me::Command& cmd, Sink&& sink);

  contracts::v1::Money compute_fee_for_fill(contracts::v1::InstrumentId instrument,
                                            contracts::v1::Price price,
                                            contracts::v1::Quantity qty,
                                            bool is_maker) const noexcept;

  contracts::v1::VenueMeta venue_;
  std::vector<contracts::v1::InstrumentMeta> instruments_;
  BookBackend backend_;
  std::unique_ptr<me::Engine<me::PoolBook>> pool_engine_;
  std::unique_ptr<me::Engine<me::MapBook>> map_engine_;

  contracts::v1::ExchangeOrderId next_exchange_order_id_ = 1;
  contracts::v1::ExecId next_exec_id_ = 1;
  std::unordered_map<contracts::v1::ExchangeOrderId, TrackedStrategyOrder> by_exchange_id_;
  std::unordered_map<contracts::v1::ClientToken, contracts::v1::ExchangeOrderId,
                     contracts::v1::ClientTokenHash>
      token_to_exchange_id_;
};

// Implements Project 03's ouch::sim::MatchingEngine virtual seam using Project 01's
// me::Engine<me::PoolBook> so OUCH ExchangeSim executes on the authoritative matching engine.
class OuchMatchingBridge final : public ouch::sim::MatchingEngine {
 public:
  explicit OuchMatchingBridge(std::vector<contracts::v1::InstrumentMeta> instruments,
                              std::uint32_t max_orders_per_book = 1U << 16U);

  void add(std::uint64_t ref, const ouch::Symbol& symbol, bool buy, std::uint32_t price,
           std::uint32_t shares, bool ioc, ouch::sim::AddResult& result) override;
  std::uint32_t reduce(std::uint64_t ref, std::uint32_t new_open) override;
  [[nodiscard]] std::uint32_t open_shares(std::uint64_t ref) const override;

  [[nodiscard]] const me::Engine<me::PoolBook>& engine() const noexcept { return *engine_; }

 private:
  std::optional<std::uint32_t> resolve_symbol(const ouch::Symbol& sym) const;

  std::vector<contracts::v1::InstrumentMeta> instruments_;
  std::unordered_map<std::string, std::uint32_t> symbol_to_id_;
  std::unordered_map<std::uint64_t, std::uint32_t> ref_to_symbol_;
  std::unique_ptr<me::Engine<me::PoolBook>> engine_;
  std::uint64_t next_match_num_ = 1;
};

}  // namespace hft::adapters
