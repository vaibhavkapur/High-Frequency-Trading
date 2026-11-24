#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <utility>

#include "lfm/cacheline.hpp"
#include "lfm/wait_policy.hpp"

namespace lfm {

template <typename Q>
concept TryQueue = requires(Q& q, const typename Q::value_type& in, typename Q::value_type& out) {
  { q.try_push(in) } -> std::convertible_to<bool>;
  { q.try_pop(out) } -> std::convertible_to<bool>;
};

template <typename W>
concept WaitPolicy = requires(W& w) {
  w.reset();
  w.wait();
};

struct ProducerStats {
  std::uint64_t pushed = 0;       // messages accepted by the queue
  std::uint64_t full_events = 0;  // times a push first found the queue full
  std::uint64_t dropped = 0;      // messages discarded by push_or_drop
  std::uint64_t wait_spins = 0;   // wait-policy iterations spent on backpressure
};

struct ConsumerStats {
  std::uint64_t popped = 0;
  std::uint64_t empty_events = 0;  // times a pop first found the queue empty
  std::uint64_t wait_spins = 0;
};

namespace detail {

// Single-writer counter. The owning thread bumps it with a plain load + store
// (no locked RMW instruction); other threads may read a slightly stale value.
class Counter {
 public:
  void add(std::uint64_t n = 1) noexcept {
    v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  std::uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> v_{0};
};

}  // namespace detail

// Producer-side handle: adds an explicit overload policy and counters on top
// of a queue's try_push. One Producer per producing thread.
//
//   try_push      - non-blocking; false on full
//   push_or_drop  - non-blocking; on full the message is dropped and counted
//   push          - applies backpressure: waits (per the wait policy) until
//                   the queue accepts the message or `should_stop` is true
template <TryQueue Q, WaitPolicy Wait = SpinThenYield>
class alignas(kCacheLine) Producer {
 public:
  using value_type = typename Q::value_type;

  explicit Producer(Q& queue, Wait wait = {}) : q_(queue), wait_(wait) {}

  [[nodiscard]] bool try_push(const value_type& v) {
    if (q_.try_push(v)) {
      pushed_.add();
      return true;
    }
    full_events_.add();
    return false;
  }

  bool push_or_drop(const value_type& v) {
    if (q_.try_push(v)) {
      pushed_.add();
      return true;
    }
    full_events_.add();
    dropped_.add();
    return false;
  }

  // Returns false only if should_stop() became true before the push succeeded.
  template <std::predicate ShouldStop>
  bool push(const value_type& v, ShouldStop&& should_stop) {
    if (q_.try_push(v)) {
      pushed_.add();
      return true;
    }
    full_events_.add();
    wait_.reset();
    std::uint64_t spins = 0;
    bool ok = false;
    while (!should_stop()) {
      wait_.wait();
      ++spins;
      if (q_.try_push(v)) {
        ok = true;
        break;
      }
    }
    wait_spins_.add(spins);
    if (ok) {
      pushed_.add();
    }
    return ok;
  }

  void push(const value_type& v) {
    push(v, [] { return false; });
  }

  // Safe to call from any thread; values may lag the producer slightly.
  [[nodiscard]] ProducerStats stats() const noexcept {
    return {pushed_.get(), full_events_.get(), dropped_.get(), wait_spins_.get()};
  }

 private:
  Q& q_;
  Wait wait_;
  detail::Counter pushed_;
  detail::Counter full_events_;
  detail::Counter dropped_;
  detail::Counter wait_spins_;
};

// Consumer-side handle. One Consumer per consuming thread.
template <TryQueue Q, WaitPolicy Wait = SpinThenYield>
class alignas(kCacheLine) Consumer {
 public:
  using value_type = typename Q::value_type;

  explicit Consumer(Q& queue, Wait wait = {}) : q_(queue), wait_(wait) {}

  [[nodiscard]] bool try_pop(value_type& out) {
    if (q_.try_pop(out)) {
      popped_.add();
      return true;
    }
    empty_events_.add();
    return false;
  }

  // Returns false only if should_stop() became true before a message arrived.
  template <std::predicate ShouldStop>
  bool pop(value_type& out, ShouldStop&& should_stop) {
    if (q_.try_pop(out)) {
      popped_.add();
      return true;
    }
    empty_events_.add();
    wait_.reset();
    std::uint64_t spins = 0;
    bool ok = false;
    while (!should_stop()) {
      wait_.wait();
      ++spins;
      if (q_.try_pop(out)) {
        ok = true;
        break;
      }
    }
    wait_spins_.add(spins);
    if (ok) {
      popped_.add();
    }
    return ok;
  }

  void pop(value_type& out) {
    pop(out, [] { return false; });
  }

  [[nodiscard]] ConsumerStats stats() const noexcept {
    return {popped_.get(), empty_events_.get(), wait_spins_.get()};
  }

 private:
  Q& q_;
  Wait wait_;
  detail::Counter popped_;
  detail::Counter empty_events_;
  detail::Counter wait_spins_;
};

}  // namespace lfm
