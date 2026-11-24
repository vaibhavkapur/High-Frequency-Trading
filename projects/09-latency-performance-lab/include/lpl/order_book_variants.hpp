#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "lpl/flat_hash_map.hpp"

namespace lpl {

enum class BookOpKind : uint8_t {
  kSetLevel = 0,
  kCancelLevel = 1,
  kQueryBest = 2,
  kQueryTopK = 3,
};

struct BookEvent {
  BookOpKind kind;
  int64_t price;
  uint64_t qty;
};

struct PriceLevel {
  int64_t price = 0;
  uint64_t qty = 0;
};

// Generates a realistic synthetic single-side (bid) order-book workload:
// most updates cluster near the top-of-book (best bid), with periodic depth walks.
inline std::vector<BookEvent> generate_order_book_workload(size_t num_events, size_t num_levels,
                                                           uint64_t seed = 1337) {
  std::vector<BookEvent> events;
  events.reserve(num_events);

  std::mt19937_64 rng(seed);
  const int64_t base_price = 100'000;
  const int64_t spread_ticks = static_cast<int64_t>(std::max<size_t>(16, num_levels));

  // Seed initial book levels.
  for (size_t i = 0; i < num_levels; ++i) {
    events.push_back(BookEvent{BookOpKind::kSetLevel, base_price - static_cast<int64_t>(i),
                               100ULL + (rng() % 900ULL)});
  }

  std::geometric_distribution<int64_t> near_bbo_dist(0.18);
  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<uint64_t> qty_dist(10, 2000);

  while (events.size() < num_events) {
    const int roll = op_dist(rng);
    const int64_t offset = near_bbo_dist(rng) % spread_ticks;
    const int64_t price = base_price - offset;

    if (roll < 55) {
      events.push_back(BookEvent{BookOpKind::kSetLevel, price, qty_dist(rng)});
    } else if (roll < 75) {
      events.push_back(BookEvent{BookOpKind::kCancelLevel, price, 0});
    } else if (roll < 92) {
      events.push_back(BookEvent{BookOpKind::kQueryBest, 0, 0});
    } else {
      events.push_back(BookEvent{BookOpKind::kQueryTopK, 0, 5});
    }
  }
  return events;
}

// 1. std::map<Price, Qty> (red-black tree, node-allocated)
class MapPriceBook {
 public:
  void clear() { levels_.clear(); }

  void set_level(int64_t price, uint64_t qty) {
    if (qty == 0) {
      levels_.erase(price);
    } else {
      levels_[price] = qty;
    }
  }

  void cancel_level(int64_t price) { levels_.erase(price); }

  PriceLevel best_bid() const noexcept {
    if (levels_.empty()) return {0, 0};
    const auto it = std::prev(levels_.end());
    return {it->first, it->second};
  }

  uint64_t top_k_volume(size_t k) const noexcept {
    uint64_t sum = 0;
    size_t count = 0;
    for (auto it = levels_.rbegin(); it != levels_.rend() && count < k; ++it, ++count) {
      sum += it->second;
    }
    return sum;
  }

  size_t num_levels() const noexcept { return levels_.size(); }

 private:
  std::map<int64_t, uint64_t> levels_;
};

// 2. Sorted contiguous vector (ascending price so highest/best bid is at back()).
// Updates near BBO touch the tail of the vector, minimizing memmove distance.
class SortedVectorPriceBook {
 public:
  void clear() { levels_.clear(); }
  void reserve(size_t n) { levels_.reserve(n); }

  void set_level(int64_t price, uint64_t qty) {
    if (qty == 0) {
      cancel_level(price);
      return;
    }
    // Reverse linear scan from back() is fast for near-BBO updates; binary search otherwise.
    auto it = find_lower_bound(price);
    if (it != levels_.end() && it->price == price) {
      it->qty = qty;
    } else {
      levels_.insert(it, PriceLevel{price, qty});
    }
  }

  void cancel_level(int64_t price) {
    auto it = find_lower_bound(price);
    if (it != levels_.end() && it->price == price) {
      levels_.erase(it);
    }
  }

  PriceLevel best_bid() const noexcept {
    if (levels_.empty()) return {0, 0};
    return levels_.back();
  }

  uint64_t top_k_volume(size_t k) const noexcept {
    uint64_t sum = 0;
    const size_t n = levels_.size();
    const size_t lim = std::min(k, n);
    for (size_t i = 0; i < lim; ++i) {
      sum += levels_[n - 1 - i].qty;
    }
    return sum;
  }

  size_t num_levels() const noexcept { return levels_.size(); }

 private:
  std::vector<PriceLevel>::iterator find_lower_bound(int64_t price) noexcept {
    return std::lower_bound(
        levels_.begin(), levels_.end(), price,
        [](const PriceLevel& lv, int64_t p) { return lv.price < p; });
  }

  std::vector<PriceLevel> levels_;
};

// 3. std::unordered_map<Price, Qty> with cached best bid (lazy rescan when best is cancelled).
class UnorderedMapPriceBook {
 public:
  void clear() {
    levels_.clear();
    best_valid_ = false;
    best_price_ = 0;
  }

  void reserve(size_t n) { levels_.reserve(n); }

  void set_level(int64_t price, uint64_t qty) {
    if (qty == 0) {
      cancel_level(price);
      return;
    }
    levels_[price] = qty;
    if (levels_.size() == 1) {
      best_price_ = price;
      best_valid_ = true;
    } else if (best_valid_ && price > best_price_) {
      best_price_ = price;
    }
  }

  void cancel_level(int64_t price) {
    const auto erased = levels_.erase(price);
    if (erased > 0 && best_valid_ && price == best_price_) {
      best_valid_ = false;
    }
  }

  PriceLevel best_bid() const noexcept {
    if (levels_.empty()) return {0, 0};
    if (!best_valid_) {
      recompute_best();
    }
    const auto it = levels_.find(best_price_);
    return (it != levels_.end()) ? PriceLevel{it->first, it->second} : PriceLevel{0, 0};
  }

  uint64_t top_k_volume(size_t k) const {
    if (levels_.empty() || k == 0) return 0;
    scratch_.clear();
    scratch_.reserve(levels_.size());
    for (const auto& [p, q] : levels_) {
      scratch_.push_back(PriceLevel{p, q});
    }
    const size_t lim = std::min(k, scratch_.size());
    std::partial_sort(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(lim),
                      scratch_.end(),
                      [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    uint64_t sum = 0;
    for (size_t i = 0; i < lim; ++i) {
      sum += scratch_[i].qty;
    }
    return sum;
  }

  size_t num_levels() const noexcept { return levels_.size(); }

 private:
  void recompute_best() const noexcept {
    int64_t best = std::numeric_limits<int64_t>::min();
    for (const auto& [p, q] : levels_) {
      (void)q;
      if (p > best) best = p;
    }
    best_price_ = best;
    best_valid_ = !levels_.empty();
  }

  std::unordered_map<int64_t, uint64_t> levels_;
  mutable int64_t best_price_ = 0;
  mutable bool best_valid_ = false;
  mutable std::vector<PriceLevel> scratch_;
};

// 4. Open-addressing FlatHashMap<Price, Qty> with cached best bid.
class FlatHashPriceBook {
 public:
  explicit FlatHashPriceBook(size_t initial_cap = 256) : levels_(initial_cap) {}

  void clear() {
    levels_.clear();
    best_valid_ = false;
    best_price_ = 0;
  }

  void reserve(size_t n) { levels_.reserve(n); }

  void set_level(int64_t price, uint64_t qty) {
    if (qty == 0) {
      cancel_level(price);
      return;
    }
    levels_.insert_or_assign(price, qty);
    if (levels_.size() == 1) {
      best_price_ = price;
      best_valid_ = true;
    } else if (best_valid_ && price > best_price_) {
      best_price_ = price;
    }
  }

  void cancel_level(int64_t price) {
    if (levels_.erase(price) && best_valid_ && price == best_price_) {
      best_valid_ = false;
    }
  }

  PriceLevel best_bid() const noexcept {
    if (levels_.empty()) return {0, 0};
    if (!best_valid_) {
      recompute_best();
    }
    const uint64_t* q = levels_.find(best_price_);
    return (q != nullptr) ? PriceLevel{best_price_, *q} : PriceLevel{0, 0};
  }

  uint64_t top_k_volume(size_t k) const {
    if (levels_.empty() || k == 0) return 0;
    scratch_.clear();
    scratch_.reserve(levels_.size());
    levels_.for_each([this](int64_t p, uint64_t q) { scratch_.push_back(PriceLevel{p, q}); });
    const size_t lim = std::min(k, scratch_.size());
    std::partial_sort(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(lim),
                      scratch_.end(),
                      [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    uint64_t sum = 0;
    for (size_t i = 0; i < lim; ++i) {
      sum += scratch_[i].qty;
    }
    return sum;
  }

  size_t num_levels() const noexcept { return levels_.size(); }

 private:
  void recompute_best() const noexcept {
    int64_t best = std::numeric_limits<int64_t>::min();
    levels_.for_each([&best](int64_t p, uint64_t) {
      if (p > best) best = p;
    });
    best_price_ = best;
    best_valid_ = !levels_.empty();
  }

  FlatHashMap<int64_t, uint64_t> levels_;
  mutable int64_t best_price_ = 0;
  mutable bool best_valid_ = false;
  mutable std::vector<PriceLevel> scratch_;
};

// Runs identical event stream through any PriceBook implementation and returns
// a deterministic checksum of all observable query outputs.
template <typename Book>
inline uint64_t replay_price_book(Book& book, const std::vector<BookEvent>& events) {
  book.clear();
  uint64_t checksum = 0;
  for (const auto& ev : events) {
    switch (ev.kind) {
      case BookOpKind::kSetLevel:
        book.set_level(ev.price, ev.qty);
        break;
      case BookOpKind::kCancelLevel:
        book.cancel_level(ev.price);
        break;
      case BookOpKind::kQueryBest: {
        const PriceLevel b = book.best_bid();
        checksum = (checksum * 1315423911ULL) ^
                   (static_cast<uint64_t>(b.price) * 31ULL + b.qty);
        break;
      }
      case BookOpKind::kQueryTopK: {
        const uint64_t vol = book.top_k_volume(static_cast<size_t>(ev.qty));
        checksum = (checksum * 1315423911ULL) ^ vol;
        break;
      }
    }
  }
  const PriceLevel final_best = book.best_bid();
  checksum ^= (static_cast<uint64_t>(final_best.price) << 16) ^ final_best.qty ^
              static_cast<uint64_t>(book.num_levels());
  return checksum;
}

}  // namespace lpl
