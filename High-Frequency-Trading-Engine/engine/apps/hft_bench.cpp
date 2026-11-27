#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "hft/runtime/pipeline.hpp"

using namespace hft::adapters;
using namespace hft::runtime;

int main(int argc, char** argv) {
  std::string out_dir = "integration/results/latest";
  std::size_t events_per_sample = 500;
  std::size_t repetitions = 5;
  std::size_t samples_per_rep = 20;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir" && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (arg == "--events" && i + 1 < argc) {
      events_per_sample = std::stoull(argv[++i]);
    } else if (arg == "--reps" && i + 1 < argc) {
      repetitions = std::stoull(argv[++i]);
    } else if (arg == "--samples" && i + 1 < argc) {
      samples_per_rep = std::stoull(argv[++i]);
    }
  }

  std::filesystem::create_directories(out_dir);
  const auto res = run_performance_suite(events_per_sample, repetitions, samples_per_rep);

  std::vector<lpl::BenchmarkResult> all_results;
  all_results.insert(all_results.end(), res.stage_benchmarks.begin(), res.stage_benchmarks.end());
  all_results.insert(all_results.end(), res.e2e_benchmarks.begin(), res.e2e_benchmarks.end());

  (void)PerfLabAdapter::write_csv_artifacts(out_dir + "/summary.csv", out_dir + "/raw_samples.csv",
                                            all_results);

  {
    std::ofstream env_f(out_dir + "/environment.json");
    env_f << PerfLabAdapter::environment_json(res.env) << "\n";
  }

  {
    std::ofstream e2e_f(out_dir + "/e2e_latency.json");
    e2e_f << std::fixed << std::setprecision(2);
    auto write_dist = [&](const char* key, const lpl::DistributionStats& d, bool comma) {
      e2e_f << "  \"" << key << "\": {"
            << "\"count\": " << d.count << ", "
            << "\"min_ns\": " << d.min << ", "
            << "\"p50_ns\": " << d.p50 << ", "
            << "\"p95_ns\": " << d.p95 << ", "
            << "\"p99_ns\": " << d.p99 << ", "
            << "\"p999_ns\": " << d.p999 << ", "
            << "\"max_ns\": " << d.max << ", "
            << "\"mean_ns\": " << d.mean << ", "
            << "\"stddev_ns\": " << d.stddev << "}" << (comma ? "," : "") << "\n";
    };
    e2e_f << "{\n";
    write_dist("tick_to_decision", res.tick_to_decision_ns, true);
    write_dist("order_submit_to_ack", res.order_submit_to_ack_ns, true);
    write_dist("order_submit_to_fill", res.order_submit_to_fill_ns, true);
    e2e_f << "  \"pool_vs_map_checksum_match\": "
          << (res.pool_vs_map_checksum_match ? "true" : "false") << ",\n"
          << "  \"direct_vs_spsc_checksum_match\": "
          << ((res.direct_checksum == res.spsc_checksum) ? "true" : "false") << ",\n"
          << "  \"spsc_drops\": " << res.spsc_drops << "\n"
          << "}\n";
  }

  std::cout << PerfLabAdapter::format_table(all_results) << "\n";
  std::cout << std::fixed << std::setprecision(1)
            << "tick_to_decision_ns:     p50=" << res.tick_to_decision_ns.p50
            << " p95=" << res.tick_to_decision_ns.p95
            << " p99=" << res.tick_to_decision_ns.p99
            << " p99.9=" << res.tick_to_decision_ns.p999
            << " max=" << res.tick_to_decision_ns.max << "\n"
            << "order_submit_to_ack_ns:  p50=" << res.order_submit_to_ack_ns.p50
            << " p95=" << res.order_submit_to_ack_ns.p95
            << " p99=" << res.order_submit_to_ack_ns.p99
            << " p99.9=" << res.order_submit_to_ack_ns.p999
            << " max=" << res.order_submit_to_ack_ns.max << "\n"
            << "order_submit_to_fill_ns: p50=" << res.order_submit_to_fill_ns.p50
            << " p95=" << res.order_submit_to_fill_ns.p95
            << " p99=" << res.order_submit_to_fill_ns.p99
            << " p99.9=" << res.order_submit_to_fill_ns.p999
            << " max=" << res.order_submit_to_fill_ns.max << "\n"
            << "Artifacts written to " << out_dir << "\n";

  return (res.pool_vs_map_checksum_match && res.direct_checksum == res.spsc_checksum &&
          res.spsc_drops == 0)
             ? 0
             : 1;
}
