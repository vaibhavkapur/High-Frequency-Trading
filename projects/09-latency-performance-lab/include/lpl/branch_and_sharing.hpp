#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "lpl/affinity.hpp"

namespace lpl {

struct BranchDataset {
  std::vector<uint32_t> sorted;
  std::vector<uint32_t> unsorted;
  uint32_t threshold = 500'000;
};

inline BranchDataset make_branch_dataset(size_t n, uint64_t seed = 777) {
  BranchDataset ds;
  ds.unsorted.resize(n);
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<uint32_t> dist(0, 999'999);
  for (size_t i = 0; i < n; ++i) {
    ds.unsorted[i] = dist(rng);
  }
  ds.sorted = ds.unsorted;
  std::sort(ds.sorted.begin(), ds.sorted.end());
  ds.threshold = 500'000;
  return ds;
}

// 1. Natural C++ `if` — compiler may auto-if-convert to `csel`/`cmov` at -O3!
__attribute__((noinline)) inline uint64_t branch_filter_natural_if(
    const std::vector<uint32_t>& data, uint32_t threshold) noexcept {
  uint64_t sum = 0;
  const size_t n = data.size();
  const uint32_t* p = data.data();
  for (size_t i = 0; i < n; ++i) {
    if (p[i] >= threshold) {
      sum += p[i];
    }
  }
  return sum;
}

// 2. Truly branchy loop: inline asm barrier inside the taken branch prevents compiler
// if-conversion into `csel`/`cmov`, forcing a real conditional branch instruction.
__attribute__((noinline)) inline uint64_t branch_filter_forced_branch(
    const std::vector<uint32_t>& data, uint32_t threshold) noexcept {
  uint64_t sum = 0;
  const size_t n = data.size();
  const uint32_t* p = data.data();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t v = p[i];
    if (v >= threshold) {
      sum += v;
#if defined(__clang__) || defined(__GNUC__)
      asm volatile("" : "+r"(sum));
#endif
    }
  }
  return sum;
}

// 3. Explicit branchless conditional accumulation using bitmask arithmetic.
__attribute__((noinline)) inline uint64_t branch_filter_branchless(
    const std::vector<uint32_t>& data, uint32_t threshold) noexcept {
  uint64_t sum = 0;
  const size_t n = data.size();
  const uint32_t* p = data.data();
  for (size_t i = 0; i < n; ++i) {
    const uint64_t v = p[i];
    const uint64_t mask = -static_cast<uint64_t>(v >= threshold);
    sum += (v & mask);
  }
  return sum;
}

// Cache-line false-sharing benchmark structures:
// Compare 8-byte packed vs 64-byte padded (x86 cache line) vs 128-byte padded (Apple Silicon /
// spatial prefetcher destructive interference granule).
template <size_t AlignBytes>
struct alignas(AlignBytes) PaddedCounter {
  std::atomic<uint64_t> value{0};
};

// Runs `num_threads` threads, each incrementing its own counter `iters_per_thread` times.
// Returns the exact sum of all counters (always num_threads * iters_per_thread).
template <size_t AlignBytes>
inline uint64_t run_false_sharing_counters(size_t num_threads, size_t iters_per_thread,
                                           bool pin_threads = false) {
  std::vector<PaddedCounter<AlignBytes>> counters(num_threads);
  std::atomic<bool> start_flag{false};
  std::atomic<size_t> ready_count{0};

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (size_t t = 0; t < num_threads; ++t) {
    workers.emplace_back([&, t]() {
      if (pin_threads) {
        (void)pin_current_thread(static_cast<int>(t));
      }
      ready_count.fetch_add(1, std::memory_order_release);
      while (!start_flag.load(std::memory_order_acquire)) {
        cpu_relax();
      }
      auto& c = counters[t].value;
      for (size_t i = 0; i < iters_per_thread; ++i) {
        c.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  while (ready_count.load(std::memory_order_acquire) < num_threads) {
    cpu_relax();
  }
  start_flag.store(true, std::memory_order_release);

  for (auto& w : workers) {
    w.join();
  }

  uint64_t total = 0;
  for (size_t t = 0; t < num_threads; ++t) {
    total += counters[t].value.load(std::memory_order_relaxed);
  }
  return total;
}

}  // namespace lpl
