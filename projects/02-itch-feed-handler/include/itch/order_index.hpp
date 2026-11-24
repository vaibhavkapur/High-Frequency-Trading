// Order reference number -> order pool slot.
//
// Two interchangeable implementations so the trade-off can be measured
// (benchmarks/itch_bench.cpp, modes book-flat and book-std):
//   * FlatOrderIndex - open addressing, linear probing, backward-shift
//     deletion, one contiguous allocation.
//   * StdOrderIndex  - std::unordered_map, one node allocation per order.
//
// Common interface:
//   const uint32_t* find(OrderRef) const;   // nullptr if absent
//   bool insert(OrderRef, uint32_t);        // false if the key already exists
//   bool erase(OrderRef);                   // false if absent
//   size_t size() const;
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

class FlatOrderIndex {
 public:
  explicit FlatOrderIndex(std::size_t initial_capacity = 1024) {
    std::size_t cap = 16;
    while (cap < initial_capacity) cap <<= 1;
    rebuild(cap);
  }

  [[nodiscard]] const std::uint32_t* find(OrderRef key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.value == kEmpty) return nullptr;
      if (s.key == key) return &s.value;
    }
  }

  bool insert(OrderRef key, std::uint32_t value) {
    assert(value != kEmpty);
    // Keep load factor <= 0.7 so probe sequences stay short and always end.
    if ((size_ + 1) * 10 > slots_.size() * 7) grow();
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.value == kEmpty) {
        s = Slot{key, value};
        ++size_;
        return true;
      }
      if (s.key == key) return false;
    }
  }

  bool erase(OrderRef key) noexcept {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.value == kEmpty) return false;
      if (s.key == key) break;
    }
    // Backward-shift deletion: pull later entries of the same probe run into
    // the hole so lookups never need tombstones.
    for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
      const Slot& s = slots_[j];
      if (s.value == kEmpty) break;
      const std::size_t k = home(s.key);
      // s may move into the hole at i only if its home is not in (i, j].
      const bool home_between = i <= j ? (i < k && k <= j) : (i < k || k <= j);
      if (home_between) continue;
      slots_[i] = s;
      i = j;
    }
    slots_[i].value = kEmpty;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  struct Slot {
    OrderRef key = 0;
    std::uint32_t value = kEmpty;
  };

  // Fibonacci hashing: order reference numbers are close to sequential, so
  // the multiply spreads them across the table and the top bits are used.
  [[nodiscard]] std::size_t home(OrderRef key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  void rebuild(std::size_t cap) {
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < cap) ++bits;
    shift_ = 64 - bits;
    size_ = 0;
  }

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    rebuild(old.size() * 2);
    for (const Slot& s : old) {
      if (s.value != kEmpty) insert(s.key, s.value);
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  unsigned shift_ = 0;
};

class StdOrderIndex {
 public:
  [[nodiscard]] const std::uint32_t* find(OrderRef key) const noexcept {
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  bool insert(OrderRef key, std::uint32_t value) { return map_.emplace(key, value).second; }
  bool erase(OrderRef key) noexcept { return map_.erase(key) != 0; }
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

 private:
  std::unordered_map<OrderRef, std::uint32_t> map_;
};

}  // namespace itch
