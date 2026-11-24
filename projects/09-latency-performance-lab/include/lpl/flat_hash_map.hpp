#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lpl {

// Open-addressing hash map with linear probing and backward-shift deletion
// (avoids tombstone accumulation under high insert/cancel churn).
// Requires an integer Key type with a sentinel empty key (default: max value).
template <typename Key, typename Value, Key EmptyKey = std::numeric_limits<Key>::max()>
class FlatHashMap {
 public:
  struct Entry {
    Key key = EmptyKey;
    Value value{};
  };

  explicit FlatHashMap(size_t initial_capacity = 64) {
    reserve_power_of_two(std::max<size_t>(16, initial_capacity));
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return table_.size(); }
  bool empty() const noexcept { return size_ == 0; }

  void clear() noexcept {
    for (auto& e : table_) {
      e.key = EmptyKey;
    }
    size_ = 0;
  }

  void reserve(size_t expected_elements) {
    const size_t min_cap = (expected_elements * 10) / 6 + 16;
    if (min_cap > table_.size()) {
      rehash(next_pow2(min_cap));
    }
  }

  Value* find(Key key) noexcept {
    if (key == EmptyKey) return nullptr;
    size_t idx = hash_key(key) & mask_;
    while (true) {
      Entry& e = table_[idx];
      if (e.key == key) return &e.value;
      if (e.key == EmptyKey) return nullptr;
      idx = (idx + 1) & mask_;
    }
  }

  const Value* find(Key key) const noexcept {
    if (key == EmptyKey) return nullptr;
    size_t idx = hash_key(key) & mask_;
    while (true) {
      const Entry& e = table_[idx];
      if (e.key == key) return &e.value;
      if (e.key == EmptyKey) return nullptr;
      idx = (idx + 1) & mask_;
    }
  }

  // Inserts or overwrites. Returns true if a new key was inserted.
  bool insert_or_assign(Key key, Value value) {
    if (key == EmptyKey) {
      throw std::invalid_argument("FlatHashMap: cannot insert reserved EmptyKey");
    }
    if ((size_ + 1) * 10 >= table_.size() * 6) {
      rehash(table_.size() * 2);
    }
    size_t idx = hash_key(key) & mask_;
    while (true) {
      Entry& e = table_[idx];
      if (e.key == EmptyKey) {
        e.key = key;
        e.value = std::move(value);
        ++size_;
        return true;
      }
      if (e.key == key) {
        e.value = std::move(value);
        return false;
      }
      idx = (idx + 1) & mask_;
    }
  }

  // Returns pointer to existing or newly default-constructed value.
  Value& operator[](Key key) {
    if (key == EmptyKey) {
      throw std::invalid_argument("FlatHashMap: cannot access reserved EmptyKey");
    }
    if ((size_ + 1) * 10 >= table_.size() * 6) {
      rehash(table_.size() * 2);
    }
    size_t idx = hash_key(key) & mask_;
    while (true) {
      Entry& e = table_[idx];
      if (e.key == EmptyKey) {
        e.key = key;
        e.value = Value{};
        ++size_;
        return e.value;
      }
      if (e.key == key) {
        return e.value;
      }
      idx = (idx + 1) & mask_;
    }
  }

  // Backward-shift deletion: keeps probe chains compact with zero tombstones.
  bool erase(Key key) noexcept {
    if (key == EmptyKey) return false;
    size_t idx = hash_key(key) & mask_;
    while (true) {
      if (table_[idx].key == EmptyKey) return false;
      if (table_[idx].key == key) break;
      idx = (idx + 1) & mask_;
    }

    --size_;
    size_t hole = idx;
    size_t cur = (hole + 1) & mask_;
    while (table_[cur].key != EmptyKey) {
      const size_t natural = hash_key(table_[cur].key) & mask_;
      // Shift cur back to hole if natural lies cyclically at or before hole relative to cur.
      const size_t dist_cur = (cur - natural) & mask_;
      const size_t dist_hole = (cur - hole) & mask_;
      if (dist_cur >= dist_hole) {
        table_[hole] = std::move(table_[cur]);
        hole = cur;
      }
      cur = (cur + 1) & mask_;
    }
    table_[hole].key = EmptyKey;
    return true;
  }

  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (const auto& e : table_) {
      if (e.key != EmptyKey) {
        fn(e.key, e.value);
      }
    }
  }

 private:
  static size_t next_pow2(size_t n) noexcept {
    if (n <= 16) return 16;
    return std::bit_ceil(n);
  }

  static size_t hash_key(Key key) noexcept {
    // SplitMix64 finalizer for strong avalanche on sequential IDs / tick prices.
    uint64_t x = static_cast<uint64_t>(key);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<size_t>(x);
  }

  void reserve_power_of_two(size_t cap) {
    const size_t p2 = next_pow2(cap);
    table_.assign(p2, Entry{});
    mask_ = p2 - 1;
    size_ = 0;
  }

  void rehash(size_t new_cap) {
    std::vector<Entry> old = std::move(table_);
    reserve_power_of_two(new_cap);
    for (auto& e : old) {
      if (e.key != EmptyKey) {
        insert_or_assign(e.key, std::move(e.value));
      }
    }
  }

  std::vector<Entry> table_;
  size_t mask_ = 0;
  size_t size_ = 0;
};

}  // namespace lpl
