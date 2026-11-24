#include "lpl/bench_harness.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace lpl {

namespace {

double percentile_sorted(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return sorted[0];
  const double rank = p * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = static_cast<size_t>(std::ceil(rank));
  const double frac = rank - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

std::string csv_escape(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
  return out;
}

}  // namespace

double calibrate_clock_overhead_ns() {
  constexpr size_t kIters = 10000;
  constexpr size_t kBatch = 100;
  std::vector<double> samples;
  samples.reserve(kIters / kBatch);
  for (size_t i = 0; i < kIters / kBatch; ++i) {
    const uint64_t t0 = steady_now_ns();
    uint64_t acc = 0;
    for (size_t j = 0; j < kBatch; ++j) {
      acc += steady_now_ns();
    }
    const uint64_t t1 = steady_now_ns();
    do_not_optimize(acc);
    samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(kBatch));
  }
  return compute_stats(std::move(samples)).p50;
}

DistributionStats compute_stats(std::vector<double> samples) {
  DistributionStats s;
  if (samples.empty()) return s;

  std::sort(samples.begin(), samples.end());
  s.count = samples.size();
  s.min = samples.front();
  s.max = samples.back();
  s.p50 = percentile_sorted(samples, 0.50);
  s.p90 = percentile_sorted(samples, 0.90);
  s.p95 = percentile_sorted(samples, 0.95);
  s.p99 = percentile_sorted(samples, 0.99);
  s.p999 = percentile_sorted(samples, 0.999);

  const double p25 = percentile_sorted(samples, 0.25);
  const double p75 = percentile_sorted(samples, 0.75);
  s.iqr = p75 - p25;

  const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  s.mean = sum / static_cast<double>(samples.size());

  if (samples.size() > 1) {
    double sq_diff = 0.0;
    for (double x : samples) {
      const double d = x - s.mean;
      sq_diff += d * d;
    }
    s.stddev = std::sqrt(sq_diff / static_cast<double>(samples.size() - 1));
    s.cv_pct = (s.mean > 0.0) ? (100.0 * s.stddev / s.mean) : 0.0;
  }
  return s;
}

BenchmarkResult run_benchmark(const std::string& suite, const std::string& name,
                              const std::string& variant, size_t workload_size,
                              const BenchmarkConfig& cfg,
                              const std::function<uint64_t()>& fn) {
  BenchmarkResult res;
  res.suite = suite;
  res.name = name;
  res.variant = variant;
  res.workload_size = workload_size;
  res.ops_per_sample = std::max<size_t>(1, cfg.ops_per_sample);
  res.repetitions = std::max<size_t>(1, cfg.repetitions);
  res.samples_per_rep = std::max<size_t>(1, cfg.samples_per_rep);

  if (cfg.pin_core >= 0) {
    (void)pin_current_thread(cfg.pin_core);
  }

  // Warmup phase (not timed).
  uint64_t warmup_sink = 0;
  for (size_t w = 0; w < cfg.warmup_rounds; ++w) {
    warmup_sink ^= fn();
  }
  do_not_optimize(warmup_sink);

  PerfCounterGroup perf;
  res.perf_backend = perf.backend_name();

  const size_t total_samples = res.repetitions * res.samples_per_rep;
  res.raw_ns_per_op.reserve(total_samples);
  res.raw_rep_index.reserve(total_samples);

  std::vector<double> rep_medians;
  rep_medians.reserve(res.repetitions);

  uint64_t total_wall_ns = 0;
  uint64_t total_cpu_ns = 0;
  uint64_t total_cycles = 0;
  uint64_t total_instructions = 0;
  uint64_t total_cache_refs = 0;
  uint64_t total_cache_misses = 0;
  uint64_t total_branches = 0;
  uint64_t total_branch_misses = 0;
  bool any_cycles = false;
  bool any_instructions = false;
  bool any_cache = false;
  bool any_branch = false;
  uint64_t last_checksum = 0;

  for (size_t rep = 0; rep < res.repetitions; ++rep) {
    std::vector<double> rep_samples;
    rep_samples.reserve(res.samples_per_rep);

    const uint64_t cpu0 = process_cpu_ns();
    const uint64_t rep_wall0 = steady_now_ns();
    perf.start();

    for (size_t s = 0; s < res.samples_per_rep; ++s) {
      clobber_memory();
      const uint64_t t0 = steady_now_ns();
      const uint64_t ck = fn();
      do_not_optimize(ck);
      clobber_memory();
      const uint64_t t1 = steady_now_ns();

      last_checksum = ck;
      const double ns_per_op =
          static_cast<double>(t1 - t0) / static_cast<double>(res.ops_per_sample);
      rep_samples.push_back(ns_per_op);
      res.raw_ns_per_op.push_back(ns_per_op);
      res.raw_rep_index.push_back(rep);
    }

    const PerfSnapshot snap = perf.stop();
    const uint64_t rep_wall1 = steady_now_ns();
    const uint64_t cpu1 = process_cpu_ns();

    total_wall_ns += (rep_wall1 - rep_wall0);
    if (cpu1 >= cpu0) {
      total_cpu_ns += (cpu1 - cpu0);
    }
    if (snap.has_cycles) {
      total_cycles += snap.cycles;
      any_cycles = true;
    }
    if (snap.has_instructions) {
      total_instructions += snap.instructions;
      any_instructions = true;
    }
    if (snap.has_cache_misses) {
      total_cache_refs += snap.cache_references;
      total_cache_misses += snap.cache_misses;
      any_cache = true;
    }
    if (snap.has_branch_misses) {
      total_branches += snap.branches;
      total_branch_misses += snap.branch_misses;
      any_branch = true;
    }

    rep_medians.push_back(compute_stats(rep_samples).p50);
  }

  res.checksum = last_checksum;
  res.ns_per_op = compute_stats(res.raw_ns_per_op);
  res.throughput_mops = (res.ns_per_op.p50 > 0.0) ? (1000.0 / res.ns_per_op.p50) : 0.0;

  const DistributionStats rep_stats = compute_stats(rep_medians);
  res.rep_p50_min = rep_stats.min;
  res.rep_p50_max = rep_stats.max;
  res.rep_p50_cv_pct = rep_stats.cv_pct;

  res.cpu_to_wall_ratio =
      (total_wall_ns > 0) ? static_cast<double>(total_cpu_ns) / static_cast<double>(total_wall_ns)
                          : 0.0;

  const double total_ops =
      static_cast<double>(total_samples) * static_cast<double>(res.ops_per_sample);
  if (any_cycles && total_ops > 0.0) {
    res.cycles_per_op = static_cast<double>(total_cycles) / total_ops;
  }
  if (any_instructions && total_ops > 0.0) {
    res.instructions_per_op = static_cast<double>(total_instructions) / total_ops;
  }
  if (any_cycles && any_instructions && total_cycles > 0) {
    res.ipc = static_cast<double>(total_instructions) / static_cast<double>(total_cycles);
  }
  if (any_cache && total_cache_refs > 0) {
    res.cache_miss_rate =
        static_cast<double>(total_cache_misses) / static_cast<double>(total_cache_refs);
  }
  if (any_branch && total_branches > 0) {
    res.branch_miss_rate =
        static_cast<double>(total_branch_misses) / static_cast<double>(total_branches);
  }

  return res;
}

std::string format_results_table(const std::vector<BenchmarkResult>& results) {
  std::ostringstream oss;
  oss << std::left << std::setw(14) << "Suite" << " | " << std::setw(20) << "Benchmark" << " | "
      << std::setw(22) << "Variant" << " | " << std::right << std::setw(9) << "p50(ns)" << " | "
      << std::setw(9) << "p99(ns)" << " | " << std::setw(9) << "Mops/s" << " | " << std::setw(7)
      << "Cyc/op" << " | " << std::setw(7) << "Ins/op" << " | " << std::setw(5) << "IPC" << " | "
      << std::setw(7) << "RepCV%" << "\n";
  oss << std::string(136, '-') << "\n";

  oss << std::fixed;
  for (const auto& r : results) {
    oss << std::left << std::setw(14) << r.suite << " | " << std::setw(20) << r.name << " | "
        << std::setw(22) << r.variant << " | " << std::right << std::setw(9) << std::setprecision(2)
        << r.ns_per_op.p50 << " | " << std::setw(9) << std::setprecision(2) << r.ns_per_op.p99
        << " | " << std::setw(9) << std::setprecision(2) << r.throughput_mops << " | ";
    if (r.cycles_per_op >= 0.0) {
      oss << std::setw(7) << std::setprecision(2) << r.cycles_per_op;
    } else {
      oss << std::setw(7) << "n/a";
    }
    oss << " | ";
    if (r.instructions_per_op >= 0.0) {
      oss << std::setw(7) << std::setprecision(2) << r.instructions_per_op;
    } else {
      oss << std::setw(7) << "n/a";
    }
    oss << " | ";
    if (r.ipc >= 0.0) {
      oss << std::setw(5) << std::setprecision(2) << r.ipc;
    } else {
      oss << std::setw(5) << "n/a";
    }
    oss << " | " << std::setw(7) << std::setprecision(2) << r.rep_p50_cv_pct << "\n";
  }
  return oss.str();
}

bool write_summary_csv(const std::string& path, const std::vector<BenchmarkResult>& results) {
  std::ofstream out(path);
  if (!out.is_open()) return false;

  out << "suite,name,variant,workload_size,ops_per_sample,repetitions,samples_per_rep,"
         "min_ns,p50_ns,p90_ns,p95_ns,p99_ns,p999_ns,max_ns,mean_ns,stddev_ns,iqr_ns,"
         "rep_p50_min_ns,rep_p50_max_ns,rep_p50_cv_pct,throughput_mops,cpu_to_wall_ratio,"
         "perf_backend,cycles_per_op,instructions_per_op,ipc,cache_miss_rate,branch_miss_rate,"
         "checksum\n";

  out << std::fixed << std::setprecision(4);
  for (const auto& r : results) {
    out << csv_escape(r.suite) << ',' << csv_escape(r.name) << ',' << csv_escape(r.variant) << ','
        << r.workload_size << ',' << r.ops_per_sample << ',' << r.repetitions << ','
        << r.samples_per_rep << ',' << r.ns_per_op.min << ',' << r.ns_per_op.p50 << ','
        << r.ns_per_op.p90 << ',' << r.ns_per_op.p95 << ',' << r.ns_per_op.p99 << ','
        << r.ns_per_op.p999 << ',' << r.ns_per_op.max << ',' << r.ns_per_op.mean << ','
        << r.ns_per_op.stddev << ',' << r.ns_per_op.iqr << ',' << r.rep_p50_min << ','
        << r.rep_p50_max << ',' << r.rep_p50_cv_pct << ',' << r.throughput_mops << ','
        << r.cpu_to_wall_ratio << ',' << csv_escape(r.perf_backend) << ',' << r.cycles_per_op << ','
        << r.instructions_per_op << ',' << r.ipc << ',' << r.cache_miss_rate << ','
        << r.branch_miss_rate << ',' << r.checksum << '\n';
  }
  return out.good();
}

bool write_raw_samples_csv(const std::string& path, const std::vector<BenchmarkResult>& results) {
  std::ofstream out(path);
  if (!out.is_open()) return false;

  out << "suite,name,variant,workload_size,rep,sample_idx,ns_per_op\n";
  out << std::fixed << std::setprecision(4);
  for (const auto& r : results) {
    for (size_t i = 0; i < r.raw_ns_per_op.size(); ++i) {
      out << csv_escape(r.suite) << ',' << csv_escape(r.name) << ',' << csv_escape(r.variant) << ','
          << r.workload_size << ',' << r.raw_rep_index[i] << ',' << i << ',' << r.raw_ns_per_op[i]
          << '\n';
    }
  }
  return out.good();
}

}  // namespace lpl
