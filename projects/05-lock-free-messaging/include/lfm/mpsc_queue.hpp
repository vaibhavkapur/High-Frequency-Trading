#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "lfm/cacheline.hpp"

namespace lfm {

// Bounded multi-producer / single-consumer queue. This is a separate
// implementation from SpscQueue, based on Dmitry Vyukov's bounded queue with
// per-slot sequence numbers, restricted to one consumer.
//
// Reservation / publication rules (invariants are spelled out in
// docs/design.md and exhaustively checked on a small model by
// tests/mpsc_model_check.cpp):
//
//   Each slot i carries a sequence number seq, initially i.
//   For a queue position `pos` mapped to slot pos & mask:
//     seq == pos            slot is free for the producer that reserves `pos`
//     seq == pos + 1        slot holds a published value for position `pos`
//     seq == pos + capacity slot was consumed; free for position pos+capacity
//
//   These states must be distinguishable, which is why capacity must be at
//   least 2: with capacity 1, "published for pos" (pos + 1) and "free for
//   pos + capacity" (pos + 1) are the same number and a producer would
//   overwrite an unread value.
//
//   reserve:  a producer that observes seq == pos claims the position by
//             CAS(tail: pos -> pos + 1). Only the CAS winner may write the
//             slot.
//   publish:  the winner constructs the value, then stores seq = pos + 1 with
//             release ordering. The consumer's acquire load of seq is what
//             makes the value visible.
//   consume:  the consumer reads slot head only when seq == head + 1, moves
//             the value out, then stores seq = head + capacity (release) to
//             hand the slot back to producers.
//
// Progress guarantees - read before calling this "lock-free":
//   * Producers are lock-free with respect to each other: a failed CAS means
//     another producer's CAS succeeded.
//   * The queue as a whole is NOT lock-free in the strict sense. A producer
//     that is descheduled between reserve and publish blocks the consumer at
//     that position (head-of-line blocking) even if later positions are
//     already published, and once the ring fills behind it, producers too.
//     No thread ever blocks in the kernel, but system-wide progress depends on
//     that producer being scheduled again.
//   * try_pop therefore returns false both when the queue is empty and when
//     the head position is reserved but not yet published.
//   * try_push may spuriously report full while the consumer is between
//     moving a value out and releasing its slot.
//
// Ordering: values from one producer are delivered in that producer's push
// order. Values from different producers are delivered in reservation (CAS)
// order.
template <typename T>
class MpscQueue {
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

 public:
  using value_type = T;

  // Throws std::invalid_argument unless `capacity` is a power of two >= 2.
  explicit MpscQueue(std::size_t capacity)
      : capacity_(checked_capacity(capacity)), mask_(capacity_ - 1), slots_(allocate(capacity)) {
    for (std::size_t i = 0; i < capacity; ++i) {
      std::construct_at(&slots_[i].seq, i);
    }
  }

  ~MpscQueue() {
    // Destroy values that were published but never consumed.
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots_[head & mask_];
      if (s.seq.load(std::memory_order_relaxed) != head + 1) {
        break;
      }
      std::destroy_at(s.value());
      ++head;
    }
    ::operator delete(static_cast<void*>(slots_), std::align_val_t{slot_alignment()});
  }

  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  // Any number of producer threads. The constructor must not throw: an
  // exception between reserve and publish would jam the queue permanently.
  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
    std::uint64_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots_[pos & mask_];
      const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
      const auto dif = static_cast<std::int64_t>(seq - pos);
      if (dif == 0) {
        // The CAS carries no data dependency: the slot hand-off is ordered by
        // seq (acquire above, release below), so relaxed is sufficient.
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
          std::construct_at(s.value(), std::forward<Args>(args)...);
          s.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS failure reloaded pos; retry with the new position.
      } else if (dif < 0) {
        return false;  // slot not yet released by the consumer: full
      } else {
        pos = tail_.load(std::memory_order_relaxed);  // lost a race; catch up
      }
    }
  }

  [[nodiscard]] bool try_push(const T& value) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    return try_emplace(value);
  }

  [[nodiscard]] bool try_push(T&& value) noexcept
    requires std::is_nothrow_move_constructible_v<T>
  {
    return try_emplace(std::move(value));
  }

  // Exactly one consumer thread.
  [[nodiscard]] bool try_pop(T& out) noexcept
    requires std::is_nothrow_move_assignable_v<T>
  {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    Slot& s = slots_[head & mask_];
    if (s.seq.load(std::memory_order_acquire) != head + 1) {
      return false;  // empty, or head position reserved but unpublished
    }
    T* p = s.value();
    out = std::move(*p);
    std::destroy_at(p);
    s.seq.store(head + capacity_, std::memory_order_release);
    // head_ is consumer-private state; it is atomic only so size_approx() can
    // read it from other threads.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(capacity_);
  }

  // Counts reserved positions (published or not). May be stale. Any thread.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(tail - head < capacity_ ? tail - head : capacity_);
  }

 private:
  struct Slot {
    std::atomic<std::uint64_t> seq;
    alignas(T) unsigned char storage[sizeof(T)];

    T* value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
  };

  static constexpr std::size_t slot_alignment() noexcept {
    return std::max(alignof(Slot), kCacheLine);
  }

  static std::uint64_t checked_capacity(std::size_t capacity) {
    if (capacity < 2 || !std::has_single_bit(capacity)) {
      throw std::invalid_argument("MpscQueue capacity must be a power of two >= 2");
    }
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot)) {
      throw std::invalid_argument("MpscQueue capacity too large");
    }
    return capacity;
  }

  static Slot* allocate(std::size_t capacity) {
    return static_cast<Slot*>(
        ::operator new(capacity * sizeof(Slot), std::align_val_t{slot_alignment()}));
  }

  alignas(kCacheLine) const std::uint64_t capacity_;
  const std::uint64_t mask_;
  Slot* const slots_;

  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};  // shared by producers
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};  // consumer-owned
};

}  // namespace lfm
