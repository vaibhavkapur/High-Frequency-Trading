// MapBook: the M1 reference-quality order book.
//
// Straightforward node-based containers: a std::map of price levels per side, a
// std::list FIFO per level, and a std::unordered_map order-id index. Every resting
// order costs heap allocations (list node + hash node, plus a map node for a new
// level). It is kept as the measured baseline for PoolBook; both books implement
// the same interface and must produce identical reports.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "me/types.hpp"

namespace me {

class MapBook {
 public:
  // max_orders is accepted for interface parity with PoolBook; the engine enforces the bound.
  explicit MapBook(std::size_t /*max_orders*/ = 0) {}

  static constexpr const char* name() noexcept { return "map"; }

  std::size_t size() const noexcept { return index_.size(); }
  bool contains(OrderId id) const { return index_.find(id) != index_.end(); }

  std::optional<OrderInfo> find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return OrderInfo{it->second.side, it->second.price, it->second.it->qty};
  }

  // Matches an incoming limit order against the opposite side (best price first,
  // FIFO within a level) and rests any remainder. Calls
  // e.trade(maker_id, price, qty, maker_leaves, taker_leaves) once per fill.
  // Precondition: !contains(id), qty > 0. Returns the quantity left resting.
  template <class Emitter>
  Qty add(OrderId id, Side side, Price price, Qty qty, Emitter& e) {
    if (side == Side::Buy) {
      qty = match(asks_, [price](Price p) { return p <= price; }, qty, e);
      if (qty > 0) rest(bids_, id, side, price, qty);
    } else {
      qty = match(bids_, [price](Price p) { return p >= price; }, qty, e);
      if (qty > 0) rest(asks_, id, side, price, qty);
    }
    return qty;
  }

  // Removes a resting order. Precondition: contains(id). Returns the quantity removed.
  Qty cancel(OrderId id) {
    const auto it = index_.find(id);
    const Locator loc = it->second;
    const Qty removed = loc.it->qty;
    if (loc.side == Side::Buy) {
      unlink(bids_, loc);
    } else {
      unlink(asks_, loc);
    }
    index_.erase(it);
    return removed;
  }

  // Lowers a resting order's quantity in place; queue position is untouched.
  // Precondition: contains(id), 0 < new_qty < current quantity. Returns the quantity removed.
  Qty reduce(OrderId id, Qty new_qty) {
    const Locator& loc = index_.find(id)->second;
    const Qty delta = loc.it->qty - new_qty;
    loc.it->qty = new_qty;
    if (loc.side == Side::Buy) {
      bids_.find(loc.price)->second.total -= delta;
    } else {
      asks_.find(loc.price)->second.total -= delta;
    }
    return delta;
  }

  std::optional<LevelView> best(Side side) const { return side == Side::Buy ? best_of(bids_) : best_of(asks_); }

  // Appends up to max_levels levels, best price first, to out.
  void depth(Side side, std::size_t max_levels, std::vector<LevelView>& out) const {
    if (side == Side::Buy) {
      depth_of(bids_, max_levels, out);
    } else {
      depth_of(asks_, max_levels, out);
    }
  }

  // Full structural self-check, O(orders). Used by tests; never on the hot path.
  bool check_invariants(std::string* why = nullptr) const {
    const auto fail = [why](const char* msg) {
      if (why != nullptr) *why = msg;
      return false;
    };
    std::size_t orders = 0;
    const auto check_side = [&](const auto& levels, Side side) -> const char* {
      for (const auto& [price, level] : levels) {
        if (level.orders.empty()) return "empty level left in book";
        Qty total = 0;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
          if (it->qty <= 0) return "resting order with non-positive quantity";
          total += it->qty;
          const auto idx = index_.find(it->id);
          if (idx == index_.end()) return "resting order missing from index";
          if (idx->second.side != side || idx->second.price != price || idx->second.it != it) {
            return "index entry does not point at its order";
          }
          ++orders;
        }
        if (total != level.total) return "level aggregate quantity is wrong";
      }
      return nullptr;
    };
    if (const char* msg = check_side(bids_, Side::Buy)) return fail(msg);
    if (const char* msg = check_side(asks_, Side::Sell)) return fail(msg);
    if (orders != index_.size()) return fail("index size differs from resting order count");
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
      return fail("book is crossed");
    }
    return true;
  }

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  using Queue = std::list<Order>;
  struct Level {
    Qty total = 0;
    Queue orders;
  };
  using Bids = std::map<Price, Level, std::greater<>>;  // best (highest) first
  using Asks = std::map<Price, Level, std::less<>>;     // best (lowest) first
  struct Locator {
    Side side;
    Price price;
    Queue::iterator it;
  };

  template <class Levels, class Crosses, class Emitter>
  Qty match(Levels& levels, Crosses crosses, Qty qty, Emitter& e) {
    while (qty > 0 && !levels.empty()) {
      const auto lit = levels.begin();
      if (!crosses(lit->first)) break;
      Level& level = lit->second;
      while (qty > 0 && !level.orders.empty()) {
        Order& maker = level.orders.front();
        const Qty fill = std::min(qty, maker.qty);
        maker.qty -= fill;
        level.total -= fill;
        qty -= fill;
        e.trade(maker.id, lit->first, fill, maker.qty, qty);
        if (maker.qty == 0) {
          index_.erase(maker.id);
          level.orders.pop_front();
        }
      }
      if (level.orders.empty()) levels.erase(lit);
    }
    return qty;
  }

  template <class Levels>
  void rest(Levels& levels, OrderId id, Side side, Price price, Qty qty) {
    Level& level = levels[price];
    level.total += qty;
    level.orders.push_back(Order{id, qty});
    index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
  }

  template <class Levels>
  void unlink(Levels& levels, const Locator& loc) {
    const auto lit = levels.find(loc.price);
    lit->second.total -= loc.it->qty;
    lit->second.orders.erase(loc.it);
    if (lit->second.orders.empty()) levels.erase(lit);
  }

  template <class Levels>
  static std::optional<LevelView> best_of(const Levels& levels) {
    if (levels.empty()) return std::nullopt;
    const auto& [price, level] = *levels.begin();
    return LevelView{price, level.total, static_cast<std::uint32_t>(level.orders.size())};
  }

  template <class Levels>
  static void depth_of(const Levels& levels, std::size_t max_levels, std::vector<LevelView>& out) {
    for (auto it = levels.begin(); it != levels.end() && max_levels > 0; ++it, --max_levels) {
      out.push_back(LevelView{it->first, it->second.total, static_cast<std::uint32_t>(it->second.orders.size())});
    }
  }

  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Locator> index_;
};

}  // namespace me
