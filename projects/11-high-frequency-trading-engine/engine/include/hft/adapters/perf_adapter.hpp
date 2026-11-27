#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hft/contracts/v1/types.hpp"
#include "lpl/affinity.hpp"
#include "lpl/bench_harness.hpp"
#include "lpl/environment.hpp"
#include "lpl/perf_counters.hpp"

namespace hft::adapters {

// Wraps Project 09 (lpl::bench_harness, lpl::environment, lpl::perf_counters) to provide
// wall-clock monotonic latency distribution measurements and machine/compiler provenance.
// Strictly uses WallTimeNs — never mixed with deterministic SimTimeNs.
class PerfLabAdapter {
 public:
  struct StageMetrics {
    std::string stage_name;
    lpl::DistributionStats ns_stats{};
  };

  [[nodiscard]] static contracts::v1::WallTimeNs now_wall_ns() noexcept {
    return contracts::v1::WallTimeNs{lpl::steady_now_ns()};
  }

  [[nodiscard]] static lpl::EnvironmentInfo collect_environment() {
    return lpl::collect_environment_info();
  }

  [[nodiscard]] static std::string environment_json(const lpl::EnvironmentInfo& env) {
    return lpl::environment_to_json(env);
  }

  [[nodiscard]] static lpl::DistributionStats summarize_ns(std::vector<double> samples_ns) {
    return lpl::compute_stats(std::move(samples_ns));
  }

  [[nodiscard]] static std::string format_table(const std::vector<lpl::BenchmarkResult>& results) {
    return lpl::format_results_table(results);
  }

  static bool write_csv_artifacts(const std::string& summary_csv_path,
                                  const std::string& raw_csv_path,
                                  const std::vector<lpl::BenchmarkResult>& results);
};

}  // namespace hft::adapters
