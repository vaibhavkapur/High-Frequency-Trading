#include "ouch/sim/matching_engine.hpp"

#include <algorithm>

namespace ouch::sim {

template <typename Levels, typename Crosses>
void SimpleMatchingEngine::match(Levels& levels, Crosses crosses, std::uint32_t& remaining,
                                 AddResult& result) {
  while (remaining > 0 && !levels.empty()) {
    auto level_it = levels.begin();
    if (!crosses(level_it->first)) break;
    Level& level = level_it->second;
    while (remaining > 0 && !level.empty()) {
      Resting& r = level.front();
      const std::uint32_t traded = std::min(remaining, r.open);
      result.fills.push_back(Fill{r.ref, traded, level_it->first, next_match_++});
      remaining -= traded;
      r.open -= traded;
      if (r.open == 0) {
        index_.erase(r.ref);
        level.pop_front();
      }
    }
    if (level.empty()) levels.erase(level_it);
  }
}

void SimpleMatchingEngine::add(std::uint64_t ref, const Symbol& symbol, bool buy,
                               std::uint32_t price, std::uint32_t shares, bool ioc,
                               AddResult& result) {
  result.fills.clear();
  result.resting = 0;
  result.canceled = 0;
  Book& book = books_[symbol];
  const bool market = price == kMarketPrice;
  std::uint32_t remaining = shares;

  if (buy) {
    match(book.asks, [&](std::uint32_t ask) { return market || ask <= price; }, remaining, result);
  } else {
    match(book.bids, [&](std::uint32_t bid) { return market || bid >= price; }, remaining, result);
  }
  if (remaining == 0) return;

  // A market order has no price to rest at, so its remainder is canceled.
  if (ioc || market) {
    result.canceled = remaining;
    return;
  }
  Locator loc{&book, buy, price, {}};
  if (buy) {
    Level& level = book.bids[price];
    loc.it = level.insert(level.end(), Resting{ref, remaining});
  } else {
    Level& level = book.asks[price];
    loc.it = level.insert(level.end(), Resting{ref, remaining});
  }
  index_.emplace(ref, loc);
  result.resting = remaining;
}

std::uint32_t SimpleMatchingEngine::reduce(std::uint64_t ref, std::uint32_t new_open) {
  auto it = index_.find(ref);
  if (it == index_.end()) return 0;
  Locator& loc = it->second;
  const std::uint32_t open = loc.it->open;
  if (new_open >= open) return 0;
  if (new_open > 0) {
    loc.it->open = new_open;
    return open - new_open;
  }
  if (loc.buy) {
    auto level = loc.book->bids.find(loc.price);
    level->second.erase(loc.it);
    if (level->second.empty()) loc.book->bids.erase(level);
  } else {
    auto level = loc.book->asks.find(loc.price);
    level->second.erase(loc.it);
    if (level->second.empty()) loc.book->asks.erase(level);
  }
  index_.erase(it);
  return open;
}

std::uint32_t SimpleMatchingEngine::open_shares(std::uint64_t ref) const {
  auto it = index_.find(ref);
  return it == index_.end() ? 0 : it->second.it->open;
}

std::uint32_t SimpleMatchingEngine::best_bid(const Symbol& symbol) const {
  auto it = books_.find(symbol);
  return it == books_.end() || it->second.bids.empty() ? 0 : it->second.bids.begin()->first;
}

std::uint32_t SimpleMatchingEngine::best_ask(const Symbol& symbol) const {
  auto it = books_.find(symbol);
  return it == books_.end() || it->second.asks.empty() ? 0 : it->second.asks.begin()->first;
}

}  // namespace ouch::sim
