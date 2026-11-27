#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hft/contracts/v1/events.hpp"

namespace hft::contracts::v1 {

struct LevelView {
  Price price{0};
  Quantity qty{0};
  std::uint32_t order_count = 0;

  constexpr bool operator==(const LevelView&) const noexcept = default;
};

struct TopOfBook {
  std::optional<LevelView> bid;
  std::optional<LevelView> ask;
  SimTimeNs last_update_ns{0};
  FeedHealth health = FeedHealth::Healthy;

  [[nodiscard]] constexpr bool valid_two_sided() const noexcept {
    return bid.has_value() && ask.has_value() && bid->price.ticks < ask->price.ticks;
  }

  [[nodiscard]] constexpr std::optional<std::int64_t> spread_ticks() const noexcept {
    if (!bid.has_value() || !ask.has_value()) return std::nullopt;
    return ask->price.ticks - bid->price.ticks;
  }
};

// Narrow simulated venue matching interface (implemented by MatchingEngineAdapter wrapping Project 01).
class IExchange {
 public:
  virtual ~IExchange() = default;

  [[nodiscard]] virtual VenueId venue_id() const noexcept = 0;

  // Apply background market liquidity events (non-strategy orders) directly to the venue truth book.
  virtual void apply_market_event(InstrumentId instrument, const MarketDataEvent& md,
                                  SimTimeNs now_ns) = 0;

  // Submit strategy order intent; populates execution reports synchronously without throwing.
  virtual ErrorCode submit_order(InstrumentId instrument, const OrderIntent& intent,
                                 SimTimeNs now_ns, std::vector<ExecutionReport>& out_reports) = 0;

  [[nodiscard]] virtual TopOfBook top_of_book(InstrumentId instrument) const = 0;
  [[nodiscard]] virtual std::vector<LevelView> depth(InstrumentId instrument, Side side,
                                                     std::size_t max_levels) const = 0;
  [[nodiscard]] virtual std::uint64_t book_hash(InstrumentId instrument) const = 0;
  [[nodiscard]] virtual bool check_invariants(std::string* why = nullptr) const = 0;
};

// Read-only observed market state exposed to strategies (NEVER future truth!).
class IObservedMarketView {
 public:
  virtual ~IObservedMarketView() = default;

  [[nodiscard]] virtual SimTimeNs now() const noexcept = 0;
  [[nodiscard]] virtual TopOfBook observed_tob(VenueId venue, InstrumentId instrument) const = 0;
  [[nodiscard]] virtual std::vector<LevelView> observed_depth(VenueId venue,
                                                              InstrumentId instrument, Side side,
                                                              std::size_t max_levels) const = 0;
  [[nodiscard]] virtual bool is_tradable(VenueId venue, InstrumentId instrument) const = 0;
  [[nodiscard]] virtual std::int64_t net_position(InstrumentId instrument) const = 0;
};

// Strategy plugin interface (implemented by simple deterministic strategy, Project 08 MM plugin,
// and Project 10 paper-arbitrage adapter).
class IStrategy {
 public:
  virtual ~IStrategy() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  virtual void on_observed_market_data(const IObservedMarketView& view, VenueId venue,
                                       InstrumentId instrument, const MarketDataEvent& md,
                                       std::vector<OrderIntent>& out_intents) = 0;
  virtual void on_execution_report(const IObservedMarketView& view, VenueId venue,
                                   InstrumentId instrument, const ExecutionReport& report,
                                   std::vector<OrderIntent>& out_intents) = 0;
};

// Bounded event transport contract (implemented by DirectTransport and SpscQueueAdapter wrapping Project 05).
struct QueueStats {
  std::uint64_t pushed = 0;
  std::uint64_t popped = 0;
  std::uint64_t rejected = 0;
  std::uint64_t dropped_for_resync = 0;
  std::uint64_t backpressure_spins = 0;
};

class IEventTransport {
 public:
  virtual ~IEventTransport() = default;

  [[nodiscard]] virtual ErrorCode push(const Envelope& env) noexcept = 0;
  [[nodiscard]] virtual bool try_pop(Envelope& out) noexcept = 0;
  [[nodiscard]] virtual QueueOverflowPolicy policy() const noexcept = 0;
  [[nodiscard]] virtual QueueStats stats() const noexcept = 0;
};

}  // namespace hft::contracts::v1
