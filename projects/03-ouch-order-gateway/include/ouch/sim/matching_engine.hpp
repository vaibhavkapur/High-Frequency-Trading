// Matching core used by the exchange simulator.
//
// MatchingEngine is the seam where the stand-alone matching-engine project
// (../01-matching-engine) plugs in. SimpleMatchingEngine is a compact
// price-time-priority limit order book that is good enough to produce
// realistic acknowledgement, fill and cancel flows for gateway testing.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "ouch/types.hpp"

namespace ouch::sim {

struct Fill {
  std::uint64_t resting_ref = 0;
  std::uint32_t shares = 0;
  std::uint32_t price = 0;  // always the resting order's price
  std::uint64_t match_number = 0;
};

struct AddResult {
  std::vector<Fill> fills;
  std::uint32_t resting = 0;   // shares left on the book
  std::uint32_t canceled = 0;  // unfilled shares of an IOC order
};

class MatchingEngine {
 public:
  virtual ~MatchingEngine() = default;

  // Matches an incoming order against the opposite side, then rests the
  // remainder unless `ioc`. `ref` must be unique.
  virtual void add(std::uint64_t ref, const Symbol& symbol, bool buy, std::uint32_t price,
                   std::uint32_t shares, bool ioc, AddResult& result) = 0;
  // Reduces a resting order to at most `new_open` shares (0 removes it).
  // Returns the number of shares removed. Priority is retained.
  virtual std::uint32_t reduce(std::uint64_t ref, std::uint32_t new_open) = 0;
  // Open shares of a resting order, or 0 if it is not on the book.
  [[nodiscard]] virtual std::uint32_t open_shares(std::uint64_t ref) const = 0;
};

class SimpleMatchingEngine final : public MatchingEngine {
 public:
  void add(std::uint64_t ref, const Symbol& symbol, bool buy, std::uint32_t price,
           std::uint32_t shares, bool ioc, AddResult& result) override;
  std::uint32_t reduce(std::uint64_t ref, std::uint32_t new_open) override;
  [[nodiscard]] std::uint32_t open_shares(std::uint64_t ref) const override;

  [[nodiscard]] std::size_t resting_orders() const { return index_.size(); }
  // Best bid / ask price for a symbol, or 0 when that side is empty.
  [[nodiscard]] std::uint32_t best_bid(const Symbol& symbol) const;
  [[nodiscard]] std::uint32_t best_ask(const Symbol& symbol) const;

 private:
  struct Resting {
    std::uint64_t ref;
    std::uint32_t open;
  };
  using Level = std::list<Resting>;
  struct Book {
    std::map<std::uint32_t, Level, std::greater<>> bids;  // best (highest) first
    std::map<std::uint32_t, Level> asks;                  // best (lowest) first
  };
  struct Locator {
    Book* book;
    bool buy;
    std::uint32_t price;
    Level::iterator it;
  };

  template <typename Levels, typename Crosses>
  void match(Levels& levels, Crosses crosses, std::uint32_t& remaining, AddResult& result);

  std::unordered_map<Symbol, Book, SymbolHash> books_;
  std::unordered_map<std::uint64_t, Locator> index_;
  std::uint64_t next_match_ = 1;
};

}  // namespace ouch::sim
