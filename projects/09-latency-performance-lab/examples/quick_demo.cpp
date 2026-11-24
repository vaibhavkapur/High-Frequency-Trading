#include <iomanip>
#include <iostream>
#include <vector>

#include "lpl/aos_soa.hpp"
#include "lpl/bench_harness.hpp"
#include "lpl/environment.hpp"
#include "lpl/matching_engine.hpp"

int main() {
  const auto env = lpl::collect_environment_info();
  std::cout << "=== Latency Performance Lab: Quick Demo ===\n";
  std::cout << lpl::environment_to_markdown(env) << "\n";

  // Quick AoS vs SoA sequential scan demo
  const size_t n = 65536;
  const auto ds = lpl::make_aos_soa_dataset(n, 42);
  lpl::BenchmarkConfig cfg{2, 3, 10, n, 0};

  const auto r_aos = lpl::run_benchmark("demo", "seq_scan", "aos_64B", n, cfg,
                                        [&]() { return lpl::scan_sequential_aos(ds.aos); });
  const auto r_soa = lpl::run_benchmark("demo", "seq_scan", "soa_split", n, cfg,
                                        [&]() { return lpl::scan_sequential_soa(ds.soa); });

  std::cout << lpl::format_results_table({r_aos, r_soa}) << "\n";
  std::cout << "Checksum match: " << (r_aos.checksum == r_soa.checksum ? "YES" : "NO") << "\n\n";

  // Quick Matching Engine A/B demo with binary encode/decode round-trip
  const auto cmds = lpl::generate_matching_workload(50'000, 4242);
  const auto wire = lpl::encode_order_stream(cmds);
  const auto decoded = lpl::decode_order_stream(wire);
  if (!decoded.ok || decoded.commands.size() != cmds.size()) {
    std::cerr << "Binary codec round-trip failed: " << decoded.error << "\n";
    return 1;
  }

  lpl::BaselineMatchingEngine eng_a;
  lpl::PooledFlatMatchingEngine eng_b(65536);

  lpl::BenchmarkConfig ab_cfg{1, 3, 3, decoded.commands.size(), 0};
  const auto r_a = lpl::run_benchmark("demo_ab", "matching_engine", "baseline_std_map",
                                      decoded.commands.size(), ab_cfg, [&]() {
                                        eng_a.reset();
                                        for (const auto& c : decoded.commands) eng_a.process(c);
                                        return eng_a.summary().trade_checksum;
                                      });
  const auto r_b = lpl::run_benchmark("demo_ab", "matching_engine", "pooled_flat",
                                      decoded.commands.size(), ab_cfg, [&]() {
                                        eng_b.reset();
                                        for (const auto& c : decoded.commands) eng_b.process(c);
                                        return eng_b.summary().trade_checksum;
                                      });

  std::cout << lpl::format_results_table({r_a, r_b}) << "\n";
  std::cout << std::fixed << std::setprecision(2)
            << "A/B checksum match: " << (r_a.checksum == r_b.checksum ? "YES" : "NO")
            << " | Measured p50 speedup: " << (r_a.ns_per_op.p50 / r_b.ns_per_op.p50) << "x\n";
  return 0;
}
