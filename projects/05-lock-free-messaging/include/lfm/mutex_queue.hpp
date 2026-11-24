#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lfm {

// Bounded FIFO guarded by one mutex and two condition variables. This is the
// reference implementation the lock-free queues are tested and benchmarked
// against; it is deliberately the obvious, textbook design. Safe for any
// number of producers and consumers.
template <typename T>
class MutexQueue {
 public:
  using value_type = T;

  explicit MutexQueue(std::size_t capacity) : buf_(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("MutexQueue capacity must be non-zero");
    }
  }

  MutexQueue(const MutexQueue&) = delete;
  MutexQueue& operator=(const MutexQueue&) = delete;

  [[nodiscard]] bool try_push(const T& value) {
    {
      std::lock_guard lock(mu_);
      if (size_ == buf_.size()) {
        return false;
      }
      put(value);
    }
    not_empty_.notify_one();
    return true;
  }

  [[nodiscard]] bool try_pop(T& out) {
    {
      std::lock_guard lock(mu_);
      if (size_ == 0) {
        return false;
      }
      take(out);
    }
    not_full_.notify_one();
    return true;
  }

  // Blocks in the kernel while the queue is full.
  void push(const T& value) {
    {
      std::unique_lock lock(mu_);
      not_full_.wait(lock, [this] { return size_ != buf_.size(); });
      put(value);
    }
    not_empty_.notify_one();
  }

  // Blocks in the kernel while the queue is empty.
  void pop(T& out) {
    {
      std::unique_lock lock(mu_);
      not_empty_.wait(lock, [this] { return size_ != 0; });
      take(out);
    }
    not_full_.notify_one();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

  [[nodiscard]] std::size_t size_approx() const {
    std::lock_guard lock(mu_);
    return size_;
  }

 private:
  void put(const T& value) {
    buf_[(head_ + size_) % buf_.size()] = value;
    ++size_;
  }

  void take(T& out) {
    out = std::move(buf_[head_]);
    head_ = (head_ + 1) % buf_.size();
    --size_;
  }

  mutable std::mutex mu_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::vector<T> buf_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

}  // namespace lfm
