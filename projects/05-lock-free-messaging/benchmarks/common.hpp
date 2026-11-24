#pragma once

// Shared plumbing for the hand-rolled benchmark harnesses (bench_throughput,
// bench_latency): clock, thread placement, argument parsing.

#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lfm/affinity.hpp"
#include "lfm/wait_policy.hpp"

namespace bench {

// Monotonic nanoseconds. Both benchmark threads live in one process and read
// the same clock, so timestamps taken on different threads are directly
// comparable ("synchronized" by construction; no cross-host sync involved).
inline std::int64_t now_ns() noexcept {
#if defined(__APPLE__)
  return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

// CPU time consumed by the calling thread.
inline std::int64_t thread_cpu_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

struct ClockInfo {
  std::int64_t resolution_ns;  // smallest non-zero step observed
  double cost_ns;              // mean cost of one now_ns() call
};

// Measured, not assumed: latency numbers below the clock's step size are
// quantisation artefacts and reports must say so.
inline ClockInfo measure_clock() {
  constexpr int kCalls = 2'000'000;
  std::int64_t min_step = INT64_MAX;
  const std::int64_t start = now_ns();
  std::int64_t prev = start;
  for (int i = 0; i < kCalls; ++i) {
    const std::int64_t t = now_ns();
    if (t != prev) {
      min_step = std::min(min_step, t - prev);
      prev = t;
    }
  }
  return {min_step, static_cast<double>(prev - start) / kCalls};
}

// Where to run a benchmark thread.
struct Placement {
  int cpu = -1;    // Linux: hard pin to this logical CPU
  char qos = 'n';  // macOS: 'p' = P-core hint, 'e' = E-core hint, 'n' = none

  // Applies the placement to the calling thread; returns what actually took
  // effect, for the results file.
  std::string apply() const {
    if (cpu >= 0) {
      return lfm::pin_current_thread(cpu) ? "cpu" + std::to_string(cpu) : "pin-failed";
    }
    if (qos == 'p' || qos == 'e') {
      const bool ok = lfm::hint_core_class(qos == 'p' ? lfm::CoreClass::kPerformance
                                                      : lfm::CoreClass::kEfficiency);
      return ok ? std::string("qos-") + qos : "qos-failed";
    }
    return "unpinned";
  }
};

// Start gate: both threads finish their setup, then are released together.
struct StartGate {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};

  void arrive_and_wait() {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
      lfm::cpu_relax();
    }
  }
  void release_when(int threads) {
    while (ready.load(std::memory_order_acquire) != threads) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
  }
};

// Tiny "--key value" parser.
class Args {
 public:
  Args(int argc, char** argv) : args_(argv + 1, argv + argc) {}

  std::string get(std::string_view key, std::string_view fallback) const {
    for (std::size_t i = 0; i + 1 < args_.size(); ++i) {
      if (args_[i] == key) {
        return args_[i + 1];
      }
    }
    return std::string(fallback);
  }
  std::int64_t get_int(std::string_view key, std::int64_t fallback) const {
    const std::string v = get(key, "");
    return v.empty() ? fallback : std::strtoll(v.c_str(), nullptr, 10);
  }
  bool has(std::string_view flag) const {
    return std::find(args_.begin(), args_.end(), flag) != args_.end();
  }

  // "8,64,256" -> {8, 64, 256}
  std::vector<std::int64_t> get_list(std::string_view key, std::string_view fallback) const {
    std::vector<std::int64_t> out;
    const std::string v = get(key, fallback);
    std::size_t pos = 0;
    while (pos < v.size()) {
      const std::size_t comma = std::min(v.find(',', pos), v.size());
      out.push_back(std::strtoll(v.substr(pos, comma - pos).c_str(), nullptr, 10));
      pos = comma + 1;
    }
    return out;
  }
  std::vector<std::string> get_names(std::string_view key, std::string_view fallback) const {
    std::vector<std::string> out;
    const std::string v = get(key, fallback);
    std::size_t pos = 0;
    while (pos < v.size()) {
      const std::size_t comma = std::min(v.find(',', pos), v.size());
      out.push_back(v.substr(pos, comma - pos));
      pos = comma + 1;
    }
    return out;
  }

 private:
  std::vector<std::string> args_;
};

inline bool contains(const std::vector<std::string>& names, std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace bench
