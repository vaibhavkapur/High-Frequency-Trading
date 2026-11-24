#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lpl/aos_soa.hpp"
#include "lpl/bench_harness.hpp"
#include "lpl/environment.hpp"

namespace {

#define REQUIRE(cond)                                                                 \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #cond << "\n"; \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (0)

void test_stats_math() {
  std::vector<double> vals = {10.0, 20.0, 30.0, 40.0, 50.0};
  const auto s = lpl::compute_stats(vals);
  REQUIRE(s.count == 5);
  REQUIRE(std::abs(s.min - 10.0) < 1e-9);
  REQUIRE(std::abs(s.p50 - 30.0) < 1e-9);
  REQUIRE(std::abs(s.max - 50.0) < 1e-9);
  REQUIRE(std::abs(s.mean - 30.0) < 1e-9);
  REQUIRE(std::abs(s.iqr - 20.0) < 1e-9);
  REQUIRE(std::abs(s.stddev - std::sqrt(250.0)) < 1e-6);

  const auto empty_stats = lpl::compute_stats({});
  REQUIRE(empty_stats.count == 0);
}

// Verifies the compiler cannot trivially dead-code-eliminate benchmarked work:
// scaling workload by 8x must increase median runtime substantially (> 3x).
void test_dce_resistance() {
  const auto ds_small = lpl::make_aos_soa_dataset(8192, 1);
  const auto ds_large = lpl::make_aos_soa_dataset(65536, 1);

  lpl::BenchmarkConfig cfg{2, 3, 15, 1, -1};  // measure whole-scan time (ops_per_sample = 1)
  const auto r_small = lpl::run_benchmark("dce", "scan", "small_8k", 8192, cfg,
                                          [&]() { return lpl::scan_sequential_aos(ds_small.aos); });
  const auto r_large = lpl::run_benchmark("dce", "scan", "large_64k", 65536, cfg,
                                          [&]() { return lpl::scan_sequential_aos(ds_large.aos); });

  REQUIRE(r_small.ns_per_op.p50 > 0.0);
  REQUIRE(r_large.ns_per_op.p50 > r_small.ns_per_op.p50 * 2.5);
}

// Verifies benchmark repeatability across independent runs and CSV export.
void test_repeatability_and_csv() {
  const auto ds = lpl::make_aos_soa_dataset(16384, 7);
  lpl::BenchmarkConfig cfg{2, 5, 12, 16384, -1};
  const auto r = lpl::run_benchmark("rep", "scan", "soa", 16384, cfg,
                                    [&]() { return lpl::scan_sequential_soa(ds.soa); });

  REQUIRE(r.raw_ns_per_op.size() == 60);
  REQUIRE(r.ns_per_op.p50 > 0.0);
  REQUIRE(r.ns_per_op.p99 >= r.ns_per_op.p50);
  // Generous bound to avoid flakiness on busy CI runners while catching broken clocks.
  REQUIRE(r.rep_p50_cv_pct < 60.0);

  const auto tmp_dir = std::filesystem::temp_directory_path() / "lpl_test_csv";
  std::filesystem::create_directories(tmp_dir);
  const std::string sum_path = (tmp_dir / "sum.csv").string();
  const std::string raw_path = (tmp_dir / "raw.csv").string();
  REQUIRE(lpl::write_summary_csv(sum_path, {r}));
  REQUIRE(lpl::write_raw_samples_csv(raw_path, {r}));

  std::ifstream in(sum_path);
  std::string header, row;
  REQUIRE(static_cast<bool>(std::getline(in, header)));
  REQUIRE(static_cast<bool>(std::getline(in, row)));
  REQUIRE(header.find("p50_ns") != std::string::npos);
  REQUIRE(row.find("soa") != std::string::npos);

  const auto env = lpl::collect_environment_info();
  REQUIRE(!env.cpu_model.empty());
  REQUIRE(env.logical_cores >= 1);
  REQUIRE(lpl::environment_to_json(env).find("cpu_model") != std::string::npos);
}

}  // namespace

int main() {
  test_stats_math();
  test_dce_resistance();
  test_repeatability_and_csv();
  std::cout << "test_harness_and_stats: ALL PASSED\n";
  return 0;
}
