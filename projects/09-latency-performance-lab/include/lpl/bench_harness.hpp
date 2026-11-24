#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lpl/affinity.hpp"
#include "lpl/perf_counters.hpp"

namespace lpl {

// Prevent the compiler from optimizing away the computation of `value`.
template <typename T>
inline void do_not_optimize(const T& value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : "r,m"(value) : "memory");
#else
  volatile const T* sink = &value;
  (void)sink;
#endif
}

template <typename T>
inline void do_not_optimize(T& value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : "+r,m"(value) : : "memory");
#else
  volatile T* sink = &value;
  (void)sink;
#endif
}

inline void clobber_memory() noexcept {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : : "memory");
#endif
}

using Clock = std::chrono::steady_clock;

inline uint64_t steady_now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

// Measures median steady_clock::now() call overhead in nanoseconds.
double calibrate_clock_overhead_ns();

struct DistributionStats {
  size_t count = 0;
  double min = 0.0;
  double p50 = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  double iqr = 0.0;
  double cv_pct = 0.0;
};

DistributionStats compute_stats(std::vector<double> samples);

struct BenchmarkConfig {
  size_t warmup_rounds = 3;
  size_t repetitions = 5;
  size_t samples_per_rep = 20;
  size_t ops_per_sample = 1;
  int pin_core = 0;
};

struct BenchmarkResult {
  std::string suite;
  std::string name;
  std::string variant;
  size_t workload_size = 0;
  size_t ops_per_sample = 1;
  size_t repetitions = 0;
  size_t samples_per_rep = 0;

  // Latency distribution across all samples (ns per operation).
  DistributionStats ns_per_op;

  // Cross-repetition stability: stats of each repetition's median ns/op.
  double rep_p50_min = 0.0;
  double rep_p50_max = 0.0;
  double rep_p50_cv_pct = 0.0;

  // Derived throughput from median ns/op.
  double throughput_mops = 0.0;

  // CPU time / wall time ratio across measured intervals (>1.0 indicates multi-core usage).
  double cpu_to_wall_ratio = 0.0;

  // Hardware counters per operation (-1 if unavailable on host OS/kernel).
  std::string perf_backend = "unavailable";
  double cycles_per_op = -1.0;
  double instructions_per_op = -1.0;
  double ipc = -1.0;
  double cache_miss_rate = -1.0;
  double branch_miss_rate = -1.0;

  // Observable checksum from the workload to verify equivalence across variants.
  uint64_t checksum = 0;

  // Raw per-sample measurements (ns/op) for CSV/distribution analysis.
  std::vector<double> raw_ns_per_op;
  std::vector<size_t> raw_rep_index;
};

// Runs a benchmark kernel `fn()` which performs `cfg.ops_per_sample` logical operations
// and returns an observable uint64_t checksum (fed to do_not_optimize).
BenchmarkResult run_benchmark(const std::string& suite, const std::string& name,
                              const std::string& variant, size_t workload_size,
                              const BenchmarkConfig& cfg,
                              const std::function<uint64_t()>& fn);

// Format a human-readable summary table to stdout/string.
std::string format_results_table(const std::vector<BenchmarkResult>& results);

// Write summary CSV and optional raw-sample CSV.
bool write_summary_csv(const std::string& path, const std::vector<BenchmarkResult>& results);
bool write_raw_samples_csv(const std::string& path, const std::vector<BenchmarkResult>& results);

}  // namespace lpl
