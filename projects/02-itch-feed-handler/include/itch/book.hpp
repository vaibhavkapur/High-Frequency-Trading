// Order-ID driven limit order book reconstruction.
//
// Model
//   * Orders live in one pool, addressed by a 32-bit slot. An order index
//     maps the ITCH order reference number to its slot. Reference numbers are
//     unique across the whole feed for a trading day (not per symbol), so the
//     index is feed-wide and each order remembers its stock locate.
//   * Each (stock locate, side) has a vector of price levels sorted from
//     worst to best, so the inside of the book is at the back where most
//     inserts and erases happen.
//   * Each level keeps its orders in an intrusive FIFO list, preserving
//     time priority. ITCH does not need this to compute depth, but it is what
//     makes replace/priority semantics observable and testable.
//
// Policy for inconsistent input (each has a counter, none is fatal):
//   * add with an order reference that is already live -> rejected.
//   * execute/cancel/delete/replace of an unknown reference -> ignored.
//   * execute/cancel for more shares than remain -> clamped, order removed.
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "itch/messages.hpp"
#include "itch/order_index.hpp"

namespace itch {

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

struct Level {
  Price4 price = 0;
  std::uint32_t order_count = 0;
  std::uint64_t qty = 0;
  std::uint32_t head = kNil;  // oldest order (highest time priority)
  std::uint32_t tail = kNil;  // newest order
};

struct LevelView {
  Price4 price = 0;
  std::uint64_t qty = 0;
  std::uint32_t orders = 0;
  friend bool operator==(const LevelView&, const LevelView&) = default;
};

struct Bbo {
  bool has_bid = false;
  bool has_ask = false;
  Price4 bid_price = 0;
  Price4 ask_price = 0;
  std::uint64_t bid_qty = 0;
  std::uint64_t ask_qty = 0;
  friend bool operator==(const Bbo&, const Bbo&) = default;
};

// Spread in Price4 units; empty unless both sides are present.
[[nodiscard]] inline std::optional<std::int64_t> spread(const Bbo& b) noexcept {
  if (!b.has_bid || !b.has_ask) return std::nullopt;
  return static_cast<std::int64_t>(b.ask_price) - static_cast<std::int64_t>(b.bid_price);
}

// Top-of-book size imbalance in [-1, 1]; positive means more bid size.
[[nodiscard]] inline std::optional<double> imbalance(const Bbo& b) noexcept {
  if (!b.has_bid || !b.has_ask || b.bid_qty + b.ask_qty == 0) return std::nullopt;
  const auto bid = static_cast<double>(b.bid_qty);
  const auto ask = static_cast<double>(b.ask_qty);
  return (bid - ask) / (bid + ask);
}

// Size-weighted mid in Price4 units: leans toward the side with less size.
[[nodiscard]] inline std::optional<double> microprice(const Bbo& b) noexcept {
  if (!b.has_bid || !b.has_ask || b.bid_qty + b.ask_qty == 0) return std::nullopt;
  const auto bid = static_cast<double>(b.bid_qty);
  const auto ask = static_cast<double>(b.ask_qty);
  return (static_cast<double>(b.bid_price) * ask + static_cast<double>(b.ask_price) * bid) /
         (bid + ask);
}

class BookSide {
 public:
  explicit BookSide(Side side) noexcept : side_(side) {}

  [[nodiscard]] Level* find(Price4 price) noexcept {
    const auto it = lower_bound(price);
    return it != levels_.end() && it->price == price ? &*it : nullptr;
  }

  [[nodiscard]] Level& find_or_insert(Price4 price) {
    auto it = lower_bound(price);
    if (it == levels_.end() || it->price != price) {
      Level lv;
      lv.price = price;
      it = levels_.insert(it, lv);
    }
    return *it;
  }

  void erase(Level* level) noexcept { levels_.erase(levels_.begin() + (level - levels_.data())); }

  [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }
  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  // i = 0 is the best price.
  [[nodiscard]] const Level& nth(std::size_t i) const noexcept {
    return levels_[levels_.size() - 1 - i];
  }

 private:
  [[nodiscard]] bool better(Price4 a, Price4 b) const noexcept {
    return side_ == Side::Buy ? a > b : a < b;
  }
  [[nodiscard]] std::vector<Level>::iterator lower_bound(Price4 price) noexcept {
    return std::lower_bound(levels_.begin(), levels_.end(), price,
                            [this](const Level& l, Price4 p) { return better(p, l.price); });
  }

  Side side_;
  std::vector<Level> levels_;  // sorted worst -> best
};

struct OrderBook {
  BookSide bids{Side::Buy};
  BookSide asks{Side::Sell};
  Symbol symbol;
  bool has_symbol = false;

  [[nodiscard]] BookSide& side(Side s) noexcept { return s == Side::Buy ? bids : asks; }
  [[nodiscard]] const BookSide& side(Side s) const noexcept { return s == Side::Buy ? bids : asks; }
};

enum class BookStatus : std::uint8_t { Ok, DuplicateOrder, UnknownOrder, OverReduce };

// What an operation did. For UnknownOrder only `status` is meaningful.
struct BookResult {
  BookStatus status = BookStatus::Ok;
  std::uint16_t locate = 0;
  Side side = Side::Buy;
  Price4 price = 0;       // price of the affected resting order
  std::uint32_t qty = 0;  // shares actually added / removed
};

struct OrderInfo {
  OrderRef ref = 0;
  std::uint16_t locate = 0;
  Side side = Side::Buy;
  Price4 price = 0;
  std::uint32_t qty = 0;
};

struct BookCounters {
  std::uint64_t adds = 0;
  std::uint64_t reduces = 0;
  std::uint64_t removes = 0;
  std::uint64_t replaces = 0;
  std::uint64_t duplicate_order = 0;
  std::uint64_t unknown_order = 0;
  std::uint64_t over_reduce = 0;
};

template <class Index = FlatOrderIndex>
class BookBuilder {
 public:
  BookResult add(OrderRef ref, std::uint16_t locate, Side side, std::uint32_t qty, Price4 price) {
    if (index_.find(ref) != nullptr) {
      ++counters_.duplicate_order;
      return {BookStatus::DuplicateOrder, locate, side, price, 0};
    }
    const std::uint32_t idx = alloc_node();
    index_.insert(ref, idx);
    Level& lv = book_mut(locate).side(side).find_or_insert(price);
    Node& n = nodes_[idx];
    n.ref = ref;
    n.qty = qty;
    n.price = price;
    n.locate = locate;
    n.side = side;
    n.next = kNil;
    n.prev = lv.tail;
    if (lv.tail != kNil) {
      nodes_[lv.tail].next = idx;
    } else {
      lv.head = idx;
    }
    lv.tail = idx;
    lv.qty += qty;
    ++lv.order_count;
    ++counters_.adds;
    return {BookStatus::Ok, locate, side, price, qty};
  }

  // Removes `qty` shares from an order (execution or partial cancel). The
  // order keeps its queue position; it leaves the book when it reaches zero.
  BookResult reduce(OrderRef ref, std::uint32_t qty) {
    const std::uint32_t* slot = index_.find(ref);
    if (slot == nullptr) return unknown();
    const std::uint32_t idx = *slot;
    Node& n = nodes_[idx];
    BookSide& side = books_[n.locate].side(n.side);
    Level* lv = side.find(n.price);
    assert(lv != nullptr);
    BookResult r{BookStatus::Ok, n.locate, n.side, n.price, qty};
    if (qty > n.qty) {
      r.qty = n.qty;
      r.status = BookStatus::OverReduce;
      ++counters_.over_reduce;
    }
    n.qty -= r.qty;
    lv->qty -= r.qty;
    ++counters_.reduces;
    if (n.qty == 0) {
      unlink(idx, lv, side);
      index_.erase(ref);
    }
    return r;
  }

  BookResult remove(OrderRef ref) {
    const std::uint32_t* slot = index_.find(ref);
    if (slot == nullptr) return unknown();
    const std::uint32_t idx = *slot;
    const Node n = nodes_[idx];
    BookSide& side = books_[n.locate].side(n.side);
    Level* lv = side.find(n.price);
    assert(lv != nullptr);
    lv->qty -= n.qty;
    unlink(idx, lv, side);
    index_.erase(ref);
    ++counters_.removes;
    return {BookStatus::Ok, n.locate, n.side, n.price, n.qty};
  }

  // ITCH replace: the original order is removed and a new order with a new
  // reference number takes its side and symbol with a new size and price.
  // The new order always goes to the back of its level's queue (it loses time
  // priority, even at an unchanged price). The original is removed even if
  // the new reference turns out to be a duplicate.
  BookResult replace(OrderRef old_ref, OrderRef new_ref, std::uint32_t qty, Price4 price) {
    const BookResult removed = remove(old_ref);
    if (removed.status != BookStatus::Ok) return removed;
    --counters_.removes;
    ++counters_.replaces;
    BookResult added = add(new_ref, removed.locate, removed.side, qty, price);
    if (added.status == BookStatus::Ok) --counters_.adds;
    return added;
  }

  void set_symbol(std::uint16_t locate, const Symbol& symbol) {
    OrderBook& b = book_mut(locate);
    b.symbol = symbol;
    b.has_symbol = true;
  }

  [[nodiscard]] std::optional<OrderInfo> find(OrderRef ref) const noexcept {
    const std::uint32_t* slot = index_.find(ref);
    if (slot == nullptr) return std::nullopt;
    const Node& n = nodes_[*slot];
    return OrderInfo{n.ref, n.locate, n.side, n.price, n.qty};
  }

  // 0-based position of an order in its price level's FIFO queue. O(queue
  // length); intended for tests and diagnostics, not the hot path.
  [[nodiscard]] std::optional<std::size_t> queue_position(OrderRef ref) const noexcept {
    const std::uint32_t* slot = index_.find(ref);
    if (slot == nullptr) return std::nullopt;
    std::size_t pos = 0;
    for (std::uint32_t i = nodes_[*slot].prev; i != kNil; i = nodes_[i].prev) ++pos;
    return pos;
  }

  [[nodiscard]] const OrderBook* book(std::uint16_t locate) const noexcept {
    return locate < books_.size() ? &books_[locate] : nullptr;
  }

  [[nodiscard]] Bbo bbo(std::uint16_t locate) const noexcept {
    Bbo out;
    const OrderBook* b = book(locate);
    if (b == nullptr) return out;
    if (!b->bids.empty()) {
      const Level& l = b->bids.nth(0);
      out.has_bid = true;
      out.bid_price = l.price;
      out.bid_qty = l.qty;
    }
    if (!b->asks.empty()) {
      const Level& l = b->asks.nth(0);
      out.has_ask = true;
      out.ask_price = l.price;
      out.ask_qty = l.qty;
    }
    return out;
  }

  // Best-first aggregated levels, at most `n` of them.
  void top(std::uint16_t locate, Side side, std::size_t n, std::vector<LevelView>& out) const {
    out.clear();
    const OrderBook* b = book(locate);
    if (b == nullptr) return;
    const BookSide& s = b->side(side);
    const std::size_t count = std::min(n, s.depth());
    for (std::size_t i = 0; i < count; ++i) {
      const Level& l = s.nth(i);
      out.push_back({l.price, l.qty, l.order_count});
    }
  }

  // Order-sensitive hash of the complete book state: every symbol, level and
  // resting order in queue order. Two replays agree on this iff they agree on
  // all prices, sizes and priorities.
  [[nodiscard]] std::uint64_t digest() const noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&h](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= 0x100000001B3ull;
      }
    };
    for (std::size_t locate = 0; locate < books_.size(); ++locate) {
      for (const Side side : {Side::Buy, Side::Sell}) {
        const BookSide& s = books_[locate].side(side);
        for (std::size_t i = 0; i < s.depth(); ++i) {
          const Level& l = s.nth(i);
          mix(locate);
          mix(static_cast<std::uint64_t>(static_cast<unsigned char>(side)));
          mix(l.price);
          mix(l.qty);
          mix(l.order_count);
          for (std::uint32_t o = l.head; o != kNil; o = nodes_[o].next) {
            mix(nodes_[o].ref);
            mix(nodes_[o].qty);
          }
        }
      }
    }
    return h;
  }

  [[nodiscard]] std::size_t live_orders() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t locate_limit() const noexcept { return books_.size(); }
  [[nodiscard]] const BookCounters& counters() const noexcept { return counters_; }

 private:
  struct Node {
    OrderRef ref = 0;
    std::uint32_t qty = 0;
    Price4 price = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;
    std::uint16_t locate = 0;
    Side side = Side::Buy;
  };

  BookResult unknown() noexcept {
    ++counters_.unknown_order;
    BookResult r;
    r.status = BookStatus::UnknownOrder;
    return r;
  }

  OrderBook& book_mut(std::uint16_t locate) {
    if (locate >= books_.size()) books_.resize(std::size_t{locate} + 1);
    return books_[locate];
  }

  std::uint32_t alloc_node() {
    if (free_head_ != kNil) {
      const std::uint32_t idx = free_head_;
      free_head_ = nodes_[idx].next;
      return idx;
    }
    assert(nodes_.size() < kNil);
    nodes_.emplace_back();
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }

  // Detaches a node from its level's queue, drops the level if it became
  // empty, and returns the node to the free list. Does not touch level qty.
  void unlink(std::uint32_t idx, Level* lv, BookSide& side) noexcept {
    Node& n = nodes_[idx];
    if (n.prev != kNil) {
      nodes_[n.prev].next = n.next;
    } else {
      lv->head = n.next;
    }
    if (n.next != kNil) {
      nodes_[n.next].prev = n.prev;
    } else {
      lv->tail = n.prev;
    }
    if (--lv->order_count == 0) side.erase(lv);
    n.next = free_head_;
    free_head_ = idx;
  }

  Index index_;
  std::vector<Node> nodes_;
  std::uint32_t free_head_ = kNil;
  std::vector<OrderBook> books_;  // indexed by stock locate
  BookCounters counters_;
};

}  // namespace itch
