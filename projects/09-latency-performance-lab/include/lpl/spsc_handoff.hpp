#pragma once

#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lpl/affinity.hpp"
#include "lpl/bench_harness.hpp"

namespace lpl {

// Cache-line padded (128B for both x86-64 and Apple Silicon) lock-free bounded SPSC ring buffer.
template <typename T>
class SpscRingBuffer {
 public:
  explicit SpscRingBuffer(size_t capacity_pow2) {
    if (capacity_pow2 < 2 || !std::has_single_bit(capacity_pow2)) {
      throw std::invalid_argument("SpscRingBuffer capacity must be a power of 2 >= 2");
    }
    mask_ = capacity_pow2 - 1;
    buffer_.resize(capacity_pow2);
  }

  size_t capacity() const noexcept { return buffer_.size(); }

  bool try_push(const T& item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) & mask_;
    if (next == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_) {
        return false;
      }
    }
    buffer_[head] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) {
        return false;
      }
    }
    out = buffer_[tail];
    tail_.store((tail + 1) & mask_, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

 private:
  static constexpr size_t kCacheLine = 128;

  size_t mask_ = 0;
  std::vector<T> buffer_;

  alignas(kCacheLine) std::atomic<size_t> head_{0};
  size_t cached_tail_ = 0;

  alignas(kCacheLine) std::atomic<size_t> tail_{0};
  size_t cached_head_ = 0;
};

enum class WaitStrategy : uint8_t {
  kBusySpin = 0,
  kHybridSpinYield = 1,
  kBlockingCondVar = 2,
};

struct HandoffMetrics {
  DistributionStats rtt_half_ns;  // One-way handoff latency estimate (RTT / 2) in ns
  double throughput_mops = 0.0;
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
  double cpu_to_wall_ratio = 0.0;  // ~2.0 for 2 busy-spinning threads, << 1.0 for idle blocking
  uint64_t checksum = 0;
};

// Measures ping-pong handoff latency (producer -> consumer -> producer) and CPU cost
// across WaitStrategy::{kBusySpin, kHybridSpinYield, kBlockingCondVar}.
// Optional `pacing_ns` injects inter-message idle delay to expose busy-spin CPU cost!
inline HandoffMetrics measure_handoff_ping_pong(WaitStrategy strategy, size_t rounds,
                                                size_t warmup_rounds = 500,
                                                uint64_t pacing_ns = 0,
                                                bool pin_threads = true) {
  HandoffMetrics out;
  std::vector<double> samples;
  samples.reserve(rounds);

  if (strategy == WaitStrategy::kBlockingCondVar) {
    std::mutex mtx;
    std::condition_variable cv_req;
    std::condition_variable cv_rsp;
    uint64_t req_val = 0;
    uint64_t rsp_val = 0;
    bool has_req = false;
    bool has_rsp = false;
    bool done = false;

    std::thread consumer([&]() {
      if (pin_threads) (void)pin_current_thread(1);
      while (true) {
        uint64_t v = 0;
        {
          std::unique_lock<std::mutex> lk(mtx);
          cv_req.wait(lk, [&] { return has_req || done; });
          if (done && !has_req) break;
          v = req_val;
          has_req = false;
        }
        {
          std::lock_guard<std::mutex> lk(mtx);
          rsp_val = v ^ 0x9e3779b97f4a7c15ULL;
          has_rsp = true;
        }
        cv_rsp.notify_one();
      }
    });

    if (pin_threads) (void)pin_current_thread(0);

    auto do_round = [&](uint64_t seq) -> uint64_t {
      {
        std::lock_guard<std::mutex> lk(mtx);
        req_val = seq;
        has_req = true;
      }
      cv_req.notify_one();
      uint64_t r = 0;
      {
        std::unique_lock<std::mutex> lk(mtx);
        cv_rsp.wait(lk, [&] { return has_rsp; });
        r = rsp_val;
        has_rsp = false;
      }
      return r;
    };

    for (size_t i = 0; i < warmup_rounds; ++i) {
      out.checksum ^= do_round(static_cast<uint64_t>(i + 1));
    }

    const uint64_t cpu0 = process_cpu_ns();
    const uint64_t wall0 = steady_now_ns();
    for (size_t i = 0; i < rounds; ++i) {
      if (pacing_ns > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(pacing_ns));
      }
      const uint64_t t0 = steady_now_ns();
      const uint64_t r = do_round(static_cast<uint64_t>(i + 1));
      const uint64_t t1 = steady_now_ns();
      out.checksum ^= r;
      samples.push_back(static_cast<double>(t1 - t0) * 0.5);
    }
    const uint64_t wall1 = steady_now_ns();
    const uint64_t cpu1 = process_cpu_ns();

    {
      std::lock_guard<std::mutex> lk(mtx);
      done = true;
    }
    cv_req.notify_one();
    consumer.join();

    out.rtt_half_ns = compute_stats(std::move(samples));
    out.wall_ms = static_cast<double>(wall1 - wall0) / 1e6;
    out.cpu_ms = static_cast<double>(cpu1 - cpu0) / 1e6;
    out.cpu_to_wall_ratio = (out.wall_ms > 0.0) ? (out.cpu_ms / out.wall_ms) : 0.0;
    out.throughput_mops = (out.rtt_half_ns.p50 > 0.0) ? (1000.0 / out.rtt_half_ns.p50) : 0.0;
    return out;
  }

  // Lock-free SPSC ping-pong (BusySpin or HybridSpinYield).
  SpscRingBuffer<uint64_t> q_req(1024);
  SpscRingBuffer<uint64_t> q_rsp(1024);
  std::atomic<bool> done{false};
  constexpr uint32_t kHybridSpinLimit = 64;

  auto wait_step = [strategy](uint32_t& spins) {
    if (strategy == WaitStrategy::kBusySpin) {
      cpu_relax();
    } else {
      if (spins < kHybridSpinLimit) {
        ++spins;
        cpu_relax();
      } else {
        std::this_thread::yield();
      }
    }
  };

  std::thread consumer([&]() {
    if (pin_threads) (void)pin_current_thread(1);
    uint64_t v = 0;
    uint32_t spins = 0;
    while (true) {
      if (q_req.try_pop(v)) {
        spins = 0;
        const uint64_t resp = v ^ 0x9e3779b97f4a7c15ULL;
        while (!q_rsp.try_push(resp)) {
          cpu_relax();
        }
      } else {
        if (done.load(std::memory_order_acquire) && q_req.empty()) break;
        wait_step(spins);
      }
    }
  });

  if (pin_threads) (void)pin_current_thread(0);

  auto do_round = [&](uint64_t seq) -> uint64_t {
    while (!q_req.try_push(seq)) {
      cpu_relax();
    }
    uint64_t resp = 0;
    uint32_t spins = 0;
    while (!q_rsp.try_pop(resp)) {
      wait_step(spins);
    }
    return resp;
  };

  for (size_t i = 0; i < warmup_rounds; ++i) {
    out.checksum ^= do_round(static_cast<uint64_t>(i + 1));
  }

  const uint64_t cpu0 = process_cpu_ns();
  const uint64_t wall0 = steady_now_ns();
  for (size_t i = 0; i < rounds; ++i) {
    if (pacing_ns > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(pacing_ns));
    }
    const uint64_t t0 = steady_now_ns();
    const uint64_t r = do_round(static_cast<uint64_t>(i + 1));
    const uint64_t t1 = steady_now_ns();
    out.checksum ^= r;
    samples.push_back(static_cast<double>(t1 - t0) * 0.5);
  }
  const uint64_t wall1 = steady_now_ns();
  const uint64_t cpu1 = process_cpu_ns();

  done.store(true, std::memory_order_release);
  consumer.join();

  out.rtt_half_ns = compute_stats(std::move(samples));
  out.wall_ms = static_cast<double>(wall1 - wall0) / 1e6;
  out.cpu_ms = static_cast<double>(cpu1 - cpu0) / 1e6;
  out.cpu_to_wall_ratio = (out.wall_ms > 0.0) ? (out.cpu_ms / out.wall_ms) : 0.0;
  out.throughput_mops = (out.rtt_half_ns.p50 > 0.0) ? (1000.0 / out.rtt_half_ns.p50) : 0.0;
  return out;
}

// Measures SPSC burst streaming throughput (producer pushes `count` items, consumer pops all).
inline uint64_t run_spsc_burst_handoff(size_t count, size_t ring_capacity = 4096,
                                       bool pin_threads = true) {
  SpscRingBuffer<uint64_t> q(ring_capacity);
  uint64_t consumer_checksum = 0;

  std::thread consumer([&]() {
    if (pin_threads) (void)pin_current_thread(1);
    uint64_t acc = 0;
    uint64_t val = 0;
    for (size_t i = 0; i < count; ++i) {
      while (!q.try_pop(val)) {
        cpu_relax();
      }
      acc += val;
    }
    consumer_checksum = acc;
  });

  if (pin_threads) (void)pin_current_thread(0);
  for (size_t i = 1; i <= count; ++i) {
    while (!q.try_push(static_cast<uint64_t>(i))) {
      cpu_relax();
    }
  }
  consumer.join();
  return consumer_checksum;
}

}  // namespace lpl
