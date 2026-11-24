#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lpl {

// Non-trivial order object with lifecycle counters to prove exact ctor/dtor pairing.
struct TrackedOrder {
  uint64_t order_id;
  int64_t price;
  uint32_t qty;
  uint32_t flags;
  uint64_t payload[4];  // 64 bytes total

  inline static int64_t live_count = 0;
  inline static uint64_t total_constructed = 0;
  inline static uint64_t total_destroyed = 0;

  static void reset_counters() noexcept {
    live_count = 0;
    total_constructed = 0;
    total_destroyed = 0;
  }

  TrackedOrder(uint64_t id, int64_t p, uint32_t q) noexcept
      : order_id(id), price(p), qty(q), flags(1), payload{id, static_cast<uint64_t>(p), q, 0} {
    ++live_count;
    ++total_constructed;
  }

  ~TrackedOrder() noexcept {
    --live_count;
    ++total_destroyed;
    flags = 0;
  }

  TrackedOrder(const TrackedOrder&) = delete;
  TrackedOrder& operator=(const TrackedOrder&) = delete;
};

// Monotonic bump-pointer arena with proper alignment and destructor invocation on reset().
class MonotonicArena {
 public:
  explicit MonotonicArena(size_t capacity_bytes)
      : capacity_(capacity_bytes),
        buffer_(static_cast<std::byte*>(::operator new(capacity_bytes, std::align_val_t{64}))) {
    if (capacity_bytes == 0) {
      throw std::invalid_argument("MonotonicArena capacity must be > 0");
    }
  }

  ~MonotonicArena() {
    reset();
    ::operator delete(buffer_, std::align_val_t{64});
  }

  MonotonicArena(const MonotonicArena&) = delete;
  MonotonicArena& operator=(const MonotonicArena&) = delete;

  template <typename T, typename... Args>
  T* create(Args&&... args) {
    const size_t align = alignof(T);
    const size_t aligned_offset = (offset_ + (align - 1)) & ~(align - 1);
    if (aligned_offset + sizeof(T) > capacity_) {
      throw std::bad_alloc();
    }
    void* ptr = buffer_ + aligned_offset;
    T* obj = ::new (ptr) T(std::forward<Args>(args)...);
    offset_ = aligned_offset + sizeof(T);
    if constexpr (!std::is_trivially_destructible_v<T>) {
      dtors_.push_back(DtorEntry{obj, [](void* p) noexcept { static_cast<T*>(p)->~T(); }});
    }
    return obj;
  }

  // Calls registered destructors in LIFO order and rewinds bump pointer in O(1) for trivial types.
  void reset() noexcept {
    for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) {
      it->fn(it->ptr);
    }
    dtors_.clear();
    offset_ = 0;
  }

  size_t bytes_used() const noexcept { return offset_; }
  size_t capacity_bytes() const noexcept { return capacity_; }

 private:
  struct DtorEntry {
    void* ptr;
    void (*fn)(void*) noexcept;
  };

  size_t capacity_ = 0;
  size_t offset_ = 0;
  std::byte* buffer_ = nullptr;
  std::vector<DtorEntry> dtors_;
};

// Fixed-capacity intrusive freelist object pool with O(1) create/destroy and full C++20 lifetime.
template <typename T>
class ObjectPool {
 public:
  explicit ObjectPool(size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("ObjectPool capacity must be > 0");
    }
    constexpr size_t kAlign = alignof(Slot);
    storage_ = static_cast<Slot*>(
        ::operator new(sizeof(Slot) * capacity_, std::align_val_t{kAlign}));
    reset_freelist();
  }

  ~ObjectPool() {
    // Caller must destroy all live objects before pool destruction.
    assert(in_use_ == 0 && "ObjectPool destroyed with live objects remaining");
    ::operator delete(storage_, std::align_val_t{alignof(Slot)});
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  template <typename... Args>
  T* create(Args&&... args) {
    if (free_head_ == nullptr) {
      return nullptr;  // Pool exhausted (caller handles gracefully without heap fallback).
    }
    Slot* slot = free_head_;
    free_head_ = slot->next_free;
    ++in_use_;
    return ::new (static_cast<void*>(slot->storage)) T(std::forward<Args>(args)...);
  }

  void destroy(T* obj) noexcept {
    if (obj == nullptr) return;
    obj->~T();
    Slot* slot = reinterpret_cast<Slot*>(obj);
    slot->next_free = free_head_;
    free_head_ = slot;
    --in_use_;
  }

  size_t in_use() const noexcept { return in_use_; }
  size_t capacity() const noexcept { return capacity_; }

 private:
  union alignas(alignof(T) > alignof(void*) ? alignof(T) : alignof(void*)) Slot {
    Slot* next_free;
    alignas(T) std::byte storage[sizeof(T)];
  };

  void reset_freelist() noexcept {
    for (size_t i = 0; i + 1 < capacity_; ++i) {
      storage_[i].next_free = &storage_[i + 1];
    }
    storage_[capacity_ - 1].next_free = nullptr;
    free_head_ = &storage_[0];
    in_use_ = 0;
  }

  size_t capacity_ = 0;
  size_t in_use_ = 0;
  Slot* storage_ = nullptr;
  Slot* free_head_ = nullptr;
};

struct AllocOp {
  bool is_alloc;
  uint32_t slot_index;
  uint64_t order_id;
  int64_t price;
  uint32_t qty;
};

// Generates deterministic order-churn trace (allocations + out-of-order cancellations).
inline std::vector<AllocOp> generate_alloc_churn_trace(size_t num_ops, size_t max_live,
                                                       uint64_t seed = 2026) {
  std::vector<AllocOp> ops;
  ops.reserve(num_ops);

  std::mt19937_64 rng(seed);
  std::vector<bool> occupied(max_live, false);
  std::vector<uint32_t> live_slots;
  live_slots.reserve(max_live);
  std::vector<uint32_t> free_slots(max_live);
  for (uint32_t i = 0; i < static_cast<uint32_t>(max_live); ++i) {
    free_slots[i] = i;
  }

  uint64_t next_id = 1;
  for (size_t i = 0; i < num_ops; ++i) {
    const bool must_alloc = live_slots.empty();
    const bool must_free = free_slots.empty();
    const bool do_alloc = must_alloc || (!must_free && (rng() % 100 < 55));

    if (do_alloc) {
      const size_t pick = rng() % free_slots.size();
      const uint32_t slot = free_slots[pick];
      free_slots[pick] = free_slots.back();
      free_slots.pop_back();
      occupied[slot] = true;
      live_slots.push_back(slot);
      ops.push_back(AllocOp{true, slot, next_id++, 10000 + static_cast<int64_t>(rng() % 500),
                            static_cast<uint32_t>(1 + (rng() % 1000))});
    } else {
      const size_t pick = rng() % live_slots.size();
      const uint32_t slot = live_slots[pick];
      live_slots[pick] = live_slots.back();
      live_slots.pop_back();
      occupied[slot] = false;
      free_slots.push_back(slot);
      ops.push_back(AllocOp{false, slot, 0, 0, 0});
    }
  }
  return ops;
}

// 1. Standard heap new/delete churn.
inline uint64_t run_heap_churn(const std::vector<AllocOp>& ops, size_t max_live) {
  std::vector<TrackedOrder*> slots(max_live, nullptr);
  uint64_t checksum = 0;
  for (const auto& op : ops) {
    if (op.is_alloc) {
      TrackedOrder* o = new TrackedOrder(op.order_id, op.price, op.qty);
      slots[op.slot_index] = o;
      checksum += o->order_id ^ static_cast<uint64_t>(o->price * o->qty);
    } else {
      TrackedOrder* o = slots[op.slot_index];
      checksum ^= o->order_id;
      delete o;
      slots[op.slot_index] = nullptr;
    }
  }
  for (TrackedOrder* o : slots) {
    if (o != nullptr) {
      checksum ^= o->order_id;
      delete o;
    }
  }
  return checksum;
}

// 2. ObjectPool<TrackedOrder> churn (supports arbitrary out-of-order destroy with O(1) reuse).
inline uint64_t run_pool_churn(ObjectPool<TrackedOrder>& pool, const std::vector<AllocOp>& ops,
                               size_t max_live) {
  std::vector<TrackedOrder*> slots(max_live, nullptr);
  uint64_t checksum = 0;
  for (const auto& op : ops) {
    if (op.is_alloc) {
      TrackedOrder* o = pool.create(op.order_id, op.price, op.qty);
      slots[op.slot_index] = o;
      checksum += o->order_id ^ static_cast<uint64_t>(o->price * o->qty);
    } else {
      TrackedOrder* o = slots[op.slot_index];
      checksum ^= o->order_id;
      pool.destroy(o);
      slots[op.slot_index] = nullptr;
    }
  }
  for (TrackedOrder* o : slots) {
    if (o != nullptr) {
      checksum ^= o->order_id;
      pool.destroy(o);
    }
  }
  return checksum;
}

// 3. MonotonicArena batch churn: allocates all orders in the batch sequentially and bulk-resets
// (calls every destructor properly on reset()). Produces the exact same checksum!
inline uint64_t run_arena_churn(MonotonicArena& arena, const std::vector<AllocOp>& ops,
                                size_t max_live) {
  arena.reset();
  std::vector<TrackedOrder*> slots(max_live, nullptr);
  uint64_t checksum = 0;
  for (const auto& op : ops) {
    if (op.is_alloc) {
      TrackedOrder* o = arena.create<TrackedOrder>(op.order_id, op.price, op.qty);
      slots[op.slot_index] = o;
      checksum += o->order_id ^ static_cast<uint64_t>(o->price * o->qty);
    } else {
      TrackedOrder* o = slots[op.slot_index];
      checksum ^= o->order_id;
      slots[op.slot_index] = nullptr;
    }
  }
  for (TrackedOrder* o : slots) {
    if (o != nullptr) {
      checksum ^= o->order_id;
    }
  }
  arena.reset();
  return checksum;
}

}  // namespace lpl
