#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace xarb {

// Lock-free Single-Producer Single-Consumer bounded ring buffer.
// Decouples asynchronous market-data receiver threads from the single-threaded book/scanner loop.
template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(size_t capacity_power_of_two)
      : capacity_(next_power_of_two(capacity_power_of_two < 2 ? 2 : capacity_power_of_two)),
        mask_(capacity_ - 1),
        buffer_(capacity_) {}

  bool try_push(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & mask_;
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;  // full
    }
    buffer_[head] = std::move(item);
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;  // empty
    }
    out = std::move(buffer_[tail]);
    tail_.store((tail + 1) & mask_, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
  }

  size_t capacity() const noexcept { return capacity_ - 1; }

 private:
  static size_t next_power_of_two(size_t n) noexcept {
    size_t p = 1;
    while (p < n) p <<= 1U;
    return p;
  }

  const size_t capacity_;
  const size_t mask_;
  std::vector<T> buffer_;

  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace xarb
