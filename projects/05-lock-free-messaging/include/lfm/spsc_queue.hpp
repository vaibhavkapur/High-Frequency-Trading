#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "lfm/cacheline.hpp"

namespace lfm {

// Bounded single-producer / single-consumer ring buffer.
//
// Ownership contract (not checked at runtime):
//   * exactly one thread at a time calls the producer API
//     (try_push / try_emplace / try_push_batch)
//   * exactly one thread at a time calls the consumer API
//     (try_pop / try_pop_batch / consume)
//   * size_approx / empty_approx / capacity may be called from any thread
//
// Semantics:
//   * capacity is a power of two and all `capacity` slots are usable
//   * every operation is non-blocking and wait-free for its caller: it runs a
//     bounded number of steps and never retries (there is no CAS loop)
//   * try_push on a full queue returns false and leaves the queue untouched;
//     unread data is never overwritten
//   * no allocation after construction
//
// Synchronisation (see docs/design.md for the full argument):
//   tail_ is written only by the producer, head_ only by the consumer. Each
//   side publishes its cursor with a release store *after* it has finished
//   touching the slot, and the other side reads it with an acquire load
//   *before* touching the slot. That release/acquire pair is the only
//   cross-thread ordering the queue needs.
//
// `CursorAlign` exists so benchmarks can instantiate a deliberately unpadded
// variant (CursorAlign = 8) and measure the cost of false sharing. Leave it at
// the default for real use.
template <typename T, std::size_t CursorAlign = kCacheLine>
class SpscQueue {
  static_assert(std::has_single_bit(CursorAlign), "CursorAlign must be a power of two");
  static_assert(CursorAlign >= alignof(std::atomic<std::uint64_t>));
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

 public:
  using value_type = T;

  // Throws std::invalid_argument unless `capacity` is a non-zero power of two.
  explicit SpscQueue(std::size_t capacity)
      : capacity_(checked_capacity(capacity)), mask_(capacity_ - 1), slots_(allocate(capacity)) {}

  ~SpscQueue() {
    if (!std::is_trivially_destructible_v<T>) {
      const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
      for (std::uint64_t i = head_.load(std::memory_order_relaxed); i != tail; ++i) {
        std::destroy_at(slot(i));
      }
    }
    ::operator delete(static_cast<void*>(slots_), std::align_val_t{slot_alignment()});
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // ---------------------------------------------------------------- producer

  template <typename... Args>
    requires std::constructible_from<T, Args...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>) {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == capacity_) {
      // Looks full according to our stale view; refresh once.
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == capacity_) {
        return false;
      }
    }
    // If the constructor throws, tail_ is not advanced and the queue is intact.
    std::construct_at(slot(tail), std::forward<Args>(args)...);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    return try_emplace(value);
  }

  [[nodiscard]] bool try_push(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    return try_emplace(std::move(value));
  }

  // Copies as many leading items as fit and publishes them with one release
  // store. Returns the number pushed (0..items.size()); partial pushes are
  // allowed.
  [[nodiscard]] std::size_t try_push_batch(std::span<const T> items) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    std::uint64_t free = capacity_ - (tail - head_cache_);
    if (free < items.size()) {
      head_cache_ = head_.load(std::memory_order_acquire);
      free = capacity_ - (tail - head_cache_);
    }
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(free, items.size()));
    if (n == 0) {
      return 0;
    }
    const std::size_t idx = static_cast<std::size_t>(tail & mask_);
    const std::size_t first = std::min<std::size_t>(n, static_cast<std::size_t>(capacity_) - idx);
    std::uninitialized_copy_n(items.data(), first, slots_ + idx);
    std::uninitialized_copy_n(items.data() + first, n - first, slots_);
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

  // ---------------------------------------------------------------- consumer

  [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    requires std::is_move_assignable_v<T>
  {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return false;
      }
    }
    T* p = slot(head);
    // If the move assignment throws, head_ is not advanced: the element stays
    // in the queue (in whatever state the failed move left it).
    out = std::move(*p);
    std::destroy_at(p);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>)
    requires std::is_move_constructible_v<T>
  {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return std::nullopt;
      }
    }
    T* p = slot(head);
    std::optional<T> out{std::move(*p)};
    std::destroy_at(p);
    head_.store(head + 1, std::memory_order_release);
    return out;
  }

  // Moves up to out.size() items into `out` and releases their slots with one
  // release store. Returns the number popped.
  [[nodiscard]] std::size_t try_pop_batch(std::span<T> out) noexcept
    requires std::is_nothrow_move_assignable_v<T>
  {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::size_t n = available(head, out.size());
    if (n == 0) {
      return 0;
    }
    const std::size_t idx = static_cast<std::size_t>(head & mask_);
    const std::size_t first = std::min<std::size_t>(n, static_cast<std::size_t>(capacity_) - idx);
    std::move(slots_ + idx, slots_ + idx + first, out.data());
    std::move(slots_, slots_ + (n - first), out.data() + first);
    std::destroy_n(slots_ + idx, first);
    std::destroy_n(slots_, n - first);
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // Calls f(T&&) for up to `max_items` queued items, in order, then releases
  // all of their slots with one release store. Returns the number consumed.
  // If f throws, the item it was handed counts as consumed and the items
  // before it are released; later items stay queued.
  template <typename F>
    requires std::invocable<F&, T&&>
  std::size_t consume(F&& f, std::size_t max_items = std::numeric_limits<std::size_t>::max()) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::size_t n = available(head, max_items);
    if (n == 0) {
      return 0;
    }
    struct Commit {
      std::atomic<std::uint64_t>& cursor;
      std::uint64_t value;
      ~Commit() { cursor.store(value, std::memory_order_release); }
    } commit{head_, head};
    for (std::size_t i = 0; i < n; ++i) {
      T* p = slot(head + i);
      struct Destroy {
        T* p;
        ~Destroy() { std::destroy_at(p); }
      } destroy{p};
      commit.value = head + i + 1;
      f(std::move(*p));
    }
    return n;
  }

  // --------------------------------------------------------------- observers

  [[nodiscard]] std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(capacity_);
  }

  // Snapshot that may be stale by the time it returns. Safe from any thread.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    // Read head first: tail never moves backwards, so tail - head >= 0. It can
    // transiently exceed capacity for an observer, hence the clamp.
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::min(tail - head, capacity_));
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

  static constexpr std::size_t cursor_alignment() noexcept { return CursorAlign; }

 private:
  static constexpr std::size_t slot_alignment() noexcept {
    return std::max(alignof(T), kCacheLine);
  }

  static std::uint64_t checked_capacity(std::size_t capacity) {
    if (!std::has_single_bit(capacity)) {
      throw std::invalid_argument("SpscQueue capacity must be a non-zero power of two");
    }
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::invalid_argument("SpscQueue capacity too large");
    }
    return capacity;
  }

  static T* allocate(std::size_t capacity) {
    return static_cast<T*>(
        ::operator new(capacity * sizeof(T), std::align_val_t{slot_alignment()}));
  }

  T* slot(std::uint64_t index) const noexcept { return slots_ + (index & mask_); }

  // Consumer side: number of items ready at `head`, capped at `want`.
  std::size_t available(std::uint64_t head, std::size_t want) noexcept {
    std::uint64_t avail = tail_cache_ - head;
    if (avail < want) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      avail = tail_cache_ - head;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(avail, want));
  }

  // Immutable after construction; read by both sides.
  alignas(CursorAlign) const std::uint64_t capacity_;
  const std::uint64_t mask_;
  T* const slots_;

  // Producer-owned line: the write cursor plus the producer's stale view of
  // the read cursor (saves an acquire load / cache miss on most pushes).
  alignas(CursorAlign) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_{0};

  // Consumer-owned line.
  alignas(CursorAlign) std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_{0};
};

}  // namespace lfm
