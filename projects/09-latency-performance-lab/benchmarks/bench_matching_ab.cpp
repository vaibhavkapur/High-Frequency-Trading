#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "lpl/affinity.hpp"
#include "lpl/bench_harness.hpp"
#include "lpl/environment.hpp"
#include "lpl/matching_engine.hpp"
#include "lpl/perf_counters.hpp"

namespace {

struct AbCli {
  std::string out_dir = "results";
  size_t num_orders = 200'000;
  size_t reps = 7;
  size_t batch_size = 32;
  int pin_core = 0;
  bool quick = false;
};

AbCli parse_cli(int argc, char** argv) {
  AbCli opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir" && i + 1 < argc) {
      opt.out_dir = argv[++i];
    } else if (arg == "--orders" && i + 1 < argc) {
      opt.num_orders = std::stoull(argv[++i]);
    } else if (arg == "--reps" && i + 1 < argc) {
      opt.reps = std::stoull(argv[++i]);
    } else if (arg == "--batch" && i + 1 < argc) {
      opt.batch_size = std::stoull(argv[++i]);
    } else if (arg == "--pin-core" && i + 1 < argc) {
      opt.pin_core = std::stoi(argv[++i]);
    } else if (arg == "--quick") {
      opt.quick = true;
      opt.num_orders = 30'000;
      opt.reps = 3;
    }
  }
  return opt;
}

template <typename Engine>
struct EngineRunOutcome {
  lpl::EngineSummary summary;
  std::vector<double> per_op_samples_ns;
  double total_wall_ns = 0.0;
  lpl::PerfSnapshot perf;
};

template <typename Engine>
EngineRunOutcome<Engine> run_engine_once(Engine& eng, const std::vector<lpl::OrderCommand>& cmds,
                                         size_t batch_size) {
  EngineRunOutcome<Engine> out;
  eng.reset();
  const size_t num_batches = cmds.size() / batch_size;
  out.per_op_samples_ns.reserve(num_batches);

  lpl::PerfCounterGroup perf;
  const uint64_t wall0 = lpl::steady_now_ns();
  perf.start();

  size_t idx = 0;
  for (size_t b = 0; b < num_batches; ++b) {
    const uint64_t t0 = lpl::steady_now_ns();
    for (size_t k = 0; k < batch_size; ++k) {
      eng.process(cmds[idx++]);
    }
    const uint64_t t1 = lpl::steady_now_ns();
    out.per_op_samples_ns.push_back(static_cast<double>(t1 - t0) /
                                    static_cast<double>(batch_size));
  }
  while (idx < cmds.size()) {
    eng.process(cmds[idx++]);
  }

  out.perf = perf.stop();
  const uint64_t wall1 = lpl::steady_now_ns();
  out.total_wall_ns = static_cast<double>(wall1 - wall0);
  out.summary = eng.summary();
  lpl::do_not_optimize(out.summary.trade_checksum);
  return out;
}

// Throughput-vs-latency load sweep (coordinated-omission aware: measures service + queueing time
// relative to scheduled arrival timestamps at target arrival rates).
struct SweepPoint {
  std::string variant;
  double target_rate_kops = 0.0;
  double achieved_rate_kops = 0.0;
  double p50_ns = 0.0;
  double p99_ns = 0.0;
  double p999_ns = 0.0;
};

template <typename Engine>
SweepPoint measure_load_point(const std::string& variant, Engine& eng,
                              const std::vector<lpl::OrderCommand>& cmds, double target_rate_kops,
                              size_t count) {
  eng.reset();
  constexpr size_t kSweepBatch = 16;
  const size_t num_batches = std::min(count, cmds.size()) / kSweepBatch;
  const size_t n = num_batches * kSweepBatch;
  const double batch_interval_ns =
      (target_rate_kops > 0.0) ? (static_cast<double>(kSweepBatch) * 1e6 / target_rate_kops) : 0.0;

  // Warmup
  for (size_t i = 0; i < std::min<size_t>(2000, n); ++i) {
    eng.process(cmds[i]);
  }
  eng.reset();

  std::vector<double> latencies_ns;
  latencies_ns.reserve(num_batches);

  const uint64_t start_ns = lpl::steady_now_ns();
  size_t idx = 0;
  for (size_t b = 0; b < num_batches; ++b) {
    const uint64_t scheduled =
        start_ns + static_cast<uint64_t>(static_cast<double>(b) * batch_interval_ns);
    if (batch_interval_ns > 0.0) {
      while (lpl::steady_now_ns() < scheduled) {
        lpl::cpu_relax();
      }
    }
    const uint64_t send_ts = (batch_interval_ns > 0.0) ? scheduled : lpl::steady_now_ns();
    for (size_t k = 0; k < kSweepBatch; ++k) {
      eng.process(cmds[idx++]);
    }
    const uint64_t done_ts = lpl::steady_now_ns();
    const double per_order_ns =
        static_cast<double>(done_ts > send_ts ? (done_ts - send_ts) : 0) /
        static_cast<double>(kSweepBatch);
    latencies_ns.push_back(per_order_ns);
  }
  const uint64_t end_ns = lpl::steady_now_ns();
  lpl::do_not_optimize(eng.summary().trade_checksum);

  const auto stats = lpl::compute_stats(std::move(latencies_ns));
  SweepPoint pt;
  pt.variant = variant;
  pt.target_rate_kops = target_rate_kops;
  pt.achieved_rate_kops =
      (end_ns > start_ns) ? (static_cast<double>(n) * 1e6 / static_cast<double>(end_ns - start_ns))
                          : 0.0;
  pt.p50_ns = stats.p50;
  pt.p99_ns = stats.p99;
  pt.p999_ns = stats.p999;
  return pt;
}

}  // namespace

int main(int argc, char** argv) {
  const AbCli opt = parse_cli(argc, argv);
  std::filesystem::create_directories(opt.out_dir);

  const auto env = lpl::collect_environment_info();
  const auto aff = lpl::pin_current_thread(opt.pin_core);

  std::cout << "=== Latency Performance Lab: Integrated Matching Engine A/B Benchmark ===\n";
  std::cout << lpl::environment_to_markdown(env);
  std::cout << "Affinity: " << aff.note << "\n";
  std::cout << "Orders: " << opt.num_orders << ", Interleaved reps: " << opt.reps
            << ", Latency batch: " << opt.batch_size << "\n\n";

  const auto cmds = lpl::generate_matching_workload(opt.num_orders, 4242);

  lpl::BaselineMatchingEngine eng_a;
  lpl::PooledFlatMatchingEngine eng_b(opt.num_orders + 1024);

  // Warmup both engines and verify bit-for-bit functional equivalence before timing.
  const auto warm_a = run_engine_once(eng_a, cmds, opt.batch_size);
  const auto warm_b = run_engine_once(eng_b, cmds, opt.batch_size);
  if (!(warm_a.summary == warm_b.summary)) {
    std::cerr << "MATCHING ENGINE EQUIVALENCE FAILURE!\n";
    return 2;
  }

  std::cout << "Equivalence verified: trades=" << warm_a.summary.trades_executed
            << ", traded_qty=" << warm_a.summary.total_traded_qty
            << ", resting=" << warm_a.summary.resting_orders
            << ", best_bid=" << warm_a.summary.best_bid
            << ", best_ask=" << warm_a.summary.best_ask
            << ", checksum=" << warm_a.summary.trade_checksum << "\n\n";

  lpl::BenchmarkResult res_a;
  res_a.suite = "matching_ab";
  res_a.name = "order_matching_e2e";
  res_a.variant = "engine_A_baseline_std_map_list";
  res_a.workload_size = opt.num_orders;
  res_a.ops_per_sample = opt.batch_size;
  res_a.repetitions = opt.reps;
  res_a.samples_per_rep = opt.num_orders / opt.batch_size;

  lpl::BenchmarkResult res_b = res_a;
  res_b.variant = "engine_B_pooled_flat_intrusive";

  std::vector<double> rep_p50_a;
  std::vector<double> rep_p50_b;
  uint64_t cyc_a = 0, ins_a = 0, cyc_b = 0, ins_b = 0;
  bool has_cyc = false, has_ins = false;

  // Interleaved ABABAB... execution to cancel thermal/background drift.
  for (size_t rep = 0; rep < opt.reps; ++rep) {
    const auto run_a = run_engine_once(eng_a, cmds, opt.batch_size);
    const auto run_b = run_engine_once(eng_b, cmds, opt.batch_size);

    if (!(run_a.summary == run_b.summary)) {
      std::cerr << "Equivalence mismatch on rep " << rep << "\n";
      return 2;
    }

    res_a.perf_backend = run_a.perf.backend;
    res_b.perf_backend = run_b.perf.backend;
    res_a.checksum = run_a.summary.trade_checksum;
    res_b.checksum = run_b.summary.trade_checksum;

    if (run_a.perf.has_cycles && run_b.perf.has_cycles) {
      cyc_a += run_a.perf.cycles;
      cyc_b += run_b.perf.cycles;
      has_cyc = true;
    }
    if (run_a.perf.has_instructions && run_b.perf.has_instructions) {
      ins_a += run_a.perf.instructions;
      ins_b += run_b.perf.instructions;
      has_ins = true;
    }

    rep_p50_a.push_back(lpl::compute_stats(run_a.per_op_samples_ns).p50);
    rep_p50_b.push_back(lpl::compute_stats(run_b.per_op_samples_ns).p50);

    for (double s : run_a.per_op_samples_ns) {
      res_a.raw_ns_per_op.push_back(s);
      res_a.raw_rep_index.push_back(rep);
    }
    for (double s : run_b.per_op_samples_ns) {
      res_b.raw_ns_per_op.push_back(s);
      res_b.raw_rep_index.push_back(rep);
    }
  }

  auto finalize = [&](lpl::BenchmarkResult& r, const std::vector<double>& rep_p50s, uint64_t cyc,
                      uint64_t ins) {
    r.ns_per_op = lpl::compute_stats(r.raw_ns_per_op);
    r.throughput_mops = (r.ns_per_op.p50 > 0.0) ? (1000.0 / r.ns_per_op.p50) : 0.0;
    const auto rs = lpl::compute_stats(rep_p50s);
    r.rep_p50_min = rs.min;
    r.rep_p50_max = rs.max;
    r.rep_p50_cv_pct = rs.cv_pct;
    const double total_ops = static_cast<double>(opt.reps * opt.num_orders);
    if (has_cyc && total_ops > 0.0) r.cycles_per_op = static_cast<double>(cyc) / total_ops;
    if (has_ins && total_ops > 0.0) r.instructions_per_op = static_cast<double>(ins) / total_ops;
    if (has_cyc && has_ins && cyc > 0) {
      r.ipc = static_cast<double>(ins) / static_cast<double>(cyc);
    }
  };

  finalize(res_a, rep_p50_a, cyc_a, ins_a);
  finalize(res_b, rep_p50_b, cyc_b, ins_b);

  const std::vector<lpl::BenchmarkResult> ab_results = {res_a, res_b};
  std::cout << lpl::format_results_table(ab_results) << "\n";

  const double speedup_p50 = res_a.ns_per_op.p50 / res_b.ns_per_op.p50;
  const double speedup_p99 = res_a.ns_per_op.p99 / res_b.ns_per_op.p99;
  std::cout << std::fixed << std::setprecision(2)
            << "A/B Summary (computed from raw samples): p50 speedup = " << speedup_p50
            << "x, p99 speedup = " << speedup_p99 << "x\n\n";

  const std::string sum_csv = opt.out_dir + "/matching_ab_summary.csv";
  const std::string raw_csv = opt.out_dir + "/matching_ab_raw.csv";
  if (!lpl::write_summary_csv(sum_csv, ab_results) ||
      !lpl::write_raw_samples_csv(raw_csv, ab_results)) {
    std::cerr << "Failed to write matching A/B CSVs\n";
    return 1;
  }

  // Throughput-vs-latency sweep
  const size_t sweep_orders = opt.quick ? 10'000 : 40'000;
  const std::vector<double> rates_kops =
      opt.quick ? std::vector<double>{500.0, 2000.0, 0.0}
                : std::vector<double>{500.0, 1000.0, 2000.0, 4000.0, 6000.0, 8000.0, 12000.0, 0.0};

  std::vector<SweepPoint> sweep;
  for (double rate : rates_kops) {
    sweep.push_back(
        measure_load_point("engine_A_baseline", eng_a, cmds, rate, sweep_orders));
    sweep.push_back(
        measure_load_point("engine_B_pooled_flat", eng_b, cmds, rate, sweep_orders));
  }

  const std::string sweep_csv = opt.out_dir + "/matching_throughput_latency.csv";
  std::ofstream sw_out(sweep_csv);
  sw_out << "variant,target_rate_kops,achieved_rate_kops,p50_ns,p99_ns,p999_ns\n";
  sw_out << std::fixed << std::setprecision(2);
  for (const auto& pt : sweep) {
    sw_out << pt.variant << ',' << pt.target_rate_kops << ',' << pt.achieved_rate_kops << ','
           << pt.p50_ns << ',' << pt.p99_ns << ',' << pt.p999_ns << '\n';
  }

  std::cout << "Wrote: " << sum_csv << "\n";
  std::cout << "Wrote: " << raw_csv << "\n";
  std::cout << "Wrote: " << sweep_csv << "\n";
  return 0;
}
