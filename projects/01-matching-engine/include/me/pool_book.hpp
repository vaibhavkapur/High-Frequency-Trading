// PoolBook: the M3 allocation-free order book.
//
// All storage is sized at construction:
//   * orders live in a preallocated pool and are addressed by 32-bit index;
//   * each price level threads its orders on an intrusive doubly linked FIFO
//     (prev/next indices stored in the order itself);
//   * each side keeps its levels in a contiguous vector sorted so that the best
//     price is at the back, where almost all inserts, erases and matches happen;
//   * the order-id index is a fixed-capacity open-addressing hash table (linear
//     probing, backward-shift deletion, load factor <= 0.5).
//
// The only operation that can allocate after construction is growth of a level
// vector beyond `expected_levels` distinct prices on one side.
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "me/types.hpp"

namespace me {

class PoolBook {
 public:
  explicit PoolBook(std::size_t max_orders, std::size_t expected_levels = 4096)
      : pool_(max_orders), slots_(std::bit_ceil(std::max<std::size_t>(16, 2 * max_orders))) {
    assert(max_orders < kNil);
    shift_ = static_cast<unsigned>(64 - std::countr_zero(slots_.size()));
    mask_ = slots_.size() - 1;
    for (std::size_t i = 0; i < pool_.size(); ++i) {
      pool_[i].next = i + 1 < pool_.size() ? static_cast<std::uint32_t>(i + 1) : kNil;
    }
    free_head_ = pool_.empty() ? kNil : 0;
    bids_.reserve(expected_levels);
    asks_.reserve(expected_levels);
  }

  static constexpr const char* name() noexcept { return "pool"; }

  std::size_t size() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return pool_.size(); }
  bool contains(OrderId id) const noexcept { return slots_[probe(id)].idx != kNil; }

  std::optional<OrderInfo> find(OrderId id) const noexcept {
    const std::uint32_t idx = slots_[probe(id)].idx;
    if (idx == kNil) return std::nullopt;
    const Order& o = pool_[idx];
    return OrderInfo{o.side, o.price, o.qty};
  }

  // Matches an incoming limit order against the opposite side (best price first,
  // FIFO within a level) and rests any remainder. Calls
  // e.trade(maker_id, price, qty, maker_leaves, taker_leaves) once per fill.
  // Precondition: !contains(id), qty > 0, and size() < capacity() if a remainder can rest.
  // Returns the quantity left resting.
  template <class Emitter>
  Qty add(OrderId id, Side side, Price price, Qty qty, Emitter& e) {
    std::vector<Level>& opp = side == Side::Buy ? asks_ : bids_;
    while (qty > 0 && !opp.empty()) {
      Level& level = opp.back();
      if (side == Side::Buy ? level.price > price : level.price < price) break;
      std::uint32_t idx = level.head;
      while (idx != kNil) {
        Order& maker = pool_[idx];
        const Qty fill = std::min(qty, maker.qty);
        maker.qty -= fill;
        level.total -= fill;
        qty -= fill;
        e.trade(maker.id, level.price, fill, maker.qty, qty);
        if (maker.qty != 0) break;  // maker partially filled, so the taker is exhausted
        const std::uint32_t next = maker.next;
        index_erase(maker.id);
        release(idx);
        --level.count;
        idx = next;
        if (qty == 0) break;
      }
      if (idx == kNil) {
        opp.pop_back();
      } else {
        level.head = idx;
        pool_[idx].prev = kNil;
      }
    }
    if (qty > 0) rest(id, side, price, qty);
    return qty;
  }

  // Removes a resting order. Precondition: contains(id). Returns the quantity removed.
  Qty cancel(OrderId id) noexcept {
    const std::size_t slot = probe(id);
    const std::uint32_t idx = slots_[slot].idx;
    const Order& o = pool_[idx];
    std::vector<Level>& levels = o.side == Side::Buy ? bids_ : asks_;
    const std::size_t pos = locate(levels, o.side, o.price);
    Level& level = levels[pos];
    const Qty removed = o.qty;
    level.total -= removed;
    if (--level.count == 0) {
      levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(pos));
    } else {
      if (o.prev != kNil) {
        pool_[o.prev].next = o.next;
      } else {
        level.head = o.next;
      }
      if (o.next != kNil) {
        pool_[o.next].prev = o.prev;
      } else {
        level.tail = o.prev;
      }
    }
    erase_slot(slot);
    release(idx);
    return removed;
  }

  // Lowers a resting order's quantity in place; queue position is untouched.
  // Precondition: contains(id), 0 < new_qty < current quantity. Returns the quantity removed.
  Qty reduce(OrderId id, Qty new_qty) noexcept {
    Order& o = pool_[slots_[probe(id)].idx];
    std::vector<Level>& levels = o.side == Side::Buy ? bids_ : asks_;
    const Qty delta = o.qty - new_qty;
    o.qty = new_qty;
    levels[locate(levels, o.side, o.price)].total -= delta;
    return delta;
  }

  std::optional<LevelView> best(Side side) const noexcept {
    const std::vector<Level>& levels = side == Side::Buy ? bids_ : asks_;
    if (levels.empty()) return std::nullopt;
    const Level& l = levels.back();
    return LevelView{l.price, l.total, l.count};
  }

  // Appends up to max_levels levels, best price first, to out.
  void depth(Side side, std::size_t max_levels, std::vector<LevelView>& out) const {
    const std::vector<Level>& levels = side == Side::Buy ? bids_ : asks_;
    for (auto it = levels.rbegin(); it != levels.rend() && max_levels > 0; ++it, --max_levels) {
      out.push_back(LevelView{it->price, it->total, it->count});
    }
  }

  // Full structural self-check, O(orders). Used by tests; never on the hot path.
  bool check_invariants(std::string* why = nullptr) const {
    const auto fail = [why](const char* msg) {
      if (why != nullptr) *why = msg;
      return false;
    };
    std::size_t orders = 0;
    const auto check_side = [&](const std::vector<Level>& levels, Side side) -> const char* {
      for (std::size_t i = 0; i < levels.size(); ++i) {
        const Level& level = levels[i];
        if (i > 0 && !less_aggressive(side, levels[i - 1].price, level.price)) {
          return "levels are not strictly sorted";
        }
        if (level.count == 0 || level.head == kNil || level.tail == kNil) {
          return "empty level left in book";
        }
        Qty total = 0;
        std::uint32_t count = 0;
        std::uint32_t prev = kNil;
        for (std::uint32_t idx = level.head; idx != kNil; idx = pool_[idx].next) {
          const Order& o = pool_[idx];
          if (o.qty <= 0) return "resting order with non-positive quantity";
          if (o.side != side || o.price != level.price) return "order threaded on the wrong level";
          if (o.prev != prev) return "broken prev link";
          if (slots_[probe(o.id)].idx != idx) return "index entry does not point at its order";
          total += o.qty;
          prev = idx;
          if (++count > level.count) return "level queue longer than its count";
        }
        if (prev != level.tail) return "level tail is wrong";
        if (count != level.count) return "level order count is wrong";
        if (total != level.total) return "level aggregate quantity is wrong";
        orders += count;
      }
      return nullptr;
    };
    if (const char* msg = check_side(bids_, Side::Buy)) return fail(msg);
    if (const char* msg = check_side(asks_, Side::Sell)) return fail(msg);
    if (orders != live_) return fail("live count differs from resting order count");
    std::size_t used_slots = 0;
    for (const Slot& s : slots_) used_slots += s.idx != kNil ? 1 : 0;
    if (used_slots != live_) return fail("index size differs from resting order count");
    std::size_t free_orders = 0;
    for (std::uint32_t idx = free_head_; idx != kNil; idx = pool_[idx].next) {
      if (++free_orders > pool_.size()) return fail("free list is cyclic");
    }
    if (free_orders + live_ != pool_.size()) return fail("pool slots leaked");
    if (!bids_.empty() && !asks_.empty() && bids_.back().price >= asks_.back().price) {
      return fail("book is crossed");
    }
    return true;
  }

 private:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;
  static constexpr int kLinearProbes = 8;

  struct Order {
    OrderId id = 0;
    Qty qty = 0;
    Price price = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;  // doubles as the free-list link while the slot is unused
    Side side = Side::Buy;
  };
  struct Level {
    Price price;
    Qty total;
    std::uint32_t head;
    std::uint32_t tail;
    std::uint32_t count;
  };
  struct Slot {
    OrderId id = 0;
    std::uint32_t idx = kNil;
  };

  // Levels are stored least aggressive first, so the best price is at the back.
  static bool less_aggressive(Side side, Price a, Price b) noexcept { return side == Side::Buy ? a < b : a > b; }

  // Index of the first level whose price is `price` or more aggressive. Activity
  // concentrates near the top of the book, so scan a few levels from the back
  // before falling back to binary search.
  static std::size_t locate(const std::vector<Level>& levels, Side side, Price price) noexcept {
    std::size_t hi = levels.size();
    for (int k = 0; k < kLinearProbes && hi > 0; ++k) {
      if (less_aggressive(side, levels[hi - 1].price, price)) return hi;
      --hi;
    }
    const auto first = levels.begin();
    const auto it = std::partition_point(first, first + static_cast<std::ptrdiff_t>(hi), [side, price](const Level& l) {
      return less_aggressive(side, l.price, price);
    });
    return static_cast<std::size_t>(it - first);
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    assert(free_head_ != kNil && "order pool exhausted; the engine must enforce max_orders");
    const std::uint32_t idx = free_head_;
    Order& o = pool_[idx];
    free_head_ = o.next;
    ++live_;
    o.id = id;
    o.qty = qty;
    o.price = price;
    o.side = side;
    o.next = kNil;

    std::vector<Level>& levels = side == Side::Buy ? bids_ : asks_;
    const std::size_t pos = locate(levels, side, price);
    if (pos < levels.size() && levels[pos].price == price) {
      Level& level = levels[pos];
      o.prev = level.tail;
      pool_[level.tail].next = idx;
      level.tail = idx;
      level.total += qty;
      ++level.count;
    } else {
      o.prev = kNil;
      levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(pos), Level{price, qty, idx, idx, 1});
    }
    Slot& slot = slots_[probe(id)];
    slot.id = id;
    slot.idx = idx;
  }

  void release(std::uint32_t idx) noexcept {
    pool_[idx].next = free_head_;
    free_head_ = idx;  // LIFO: the next resting order reuses the most recently touched slot
    --live_;
  }

  std::size_t home(OrderId id) const noexcept {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  // Slot holding `id`, or the empty slot where it would be inserted.
  std::size_t probe(OrderId id) const noexcept {
    std::size_t i = home(id);
    while (slots_[i].idx != kNil && slots_[i].id != id) i = (i + 1) & mask_;
    return i;
  }

  void index_erase(OrderId id) noexcept { erase_slot(probe(id)); }

  // Backward-shift deletion keeps probe sequences intact without tombstones.
  void erase_slot(std::size_t i) noexcept {
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].idx == kNil) break;
      const std::size_t k = home(slots_[j].id);
      const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].idx = kNil;
  }

  std::vector<Order> pool_;
  std::vector<Slot> slots_;
  std::vector<Level> bids_;  // ascending price: best bid at the back
  std::vector<Level> asks_;  // descending price: best ask at the back
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::uint32_t free_head_ = kNil;
  std::uint32_t live_ = 0;
};

}  // namespace me
