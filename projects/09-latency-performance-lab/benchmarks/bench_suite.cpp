#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lpl/affinity.hpp"
#include "lpl/allocator_bench.hpp"
#include "lpl/aos_soa.hpp"
#include "lpl/bench_harness.hpp"
#include "lpl/branch_and_sharing.hpp"
#include "lpl/dispatch.hpp"
#include "lpl/environment.hpp"
#include "lpl/order_book_variants.hpp"
#include "lpl/spsc_handoff.hpp"

namespace {

struct CliOptions {
  std::string out_dir = "results";
  std::string suite = "all";
  size_t reps = 7;
  size_t samples = 25;
  size_t warmup = 3;
  int pin_core = 0;
  bool quick = false;
};

CliOptions parse_cli(int argc, char** argv) {
  CliOptions opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir" && i + 1 < argc) {
      opt.out_dir = argv[++i];
    } else if (arg == "--suite" && i + 1 < argc) {
      opt.suite = argv[++i];
    } else if (arg == "--reps" && i + 1 < argc) {
      opt.reps = std::stoull(argv[++i]);
    } else if (arg == "--samples" && i + 1 < argc) {
      opt.samples = std::stoull(argv[++i]);
    } else if (arg == "--warmup" && i + 1 < argc) {
      opt.warmup = std::stoull(argv[++i]);
    } else if (arg == "--pin-core" && i + 1 < argc) {
      opt.pin_core = std::stoi(argv[++i]);
    } else if (arg == "--quick") {
      opt.quick = true;
      opt.reps = 3;
      opt.samples = 8;
      opt.warmup = 1;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: bench_suite [--out-dir DIR] [--suite all|aos_soa|order_book|"
                   "allocator|dispatch|branch_sharing|spsc] [--reps N] [--samples N] "
                   "[--warmup N] [--pin-core C] [--quick]\n";
      std::exit(0);
    }
  }
  return opt;
}

void assert_equal_checksum(const std::string& label, uint64_t a, uint64_t b) {
  if (a != b) {
    std::cerr << "EQUIVALENCE FAILURE in " << label << ": " << a << " != " << b << "\n";
    std::exit(2);
  }
}

bool want_suite(const CliOptions& opt, const std::string& name) {
  return opt.suite == "all" || opt.suite == name;
}

lpl::BenchmarkResult make_handoff_result(const std::string& name, const std::string& variant,
                                         size_t workload_size, size_t reps,
                                         const std::vector<lpl::HandoffMetrics>& runs) {
  lpl::BenchmarkResult r;
  r.suite = "spsc";
  r.name = name;
  r.variant = variant;
  r.workload_size = workload_size;
  r.ops_per_sample = 1;
  r.repetitions = reps;
  r.samples_per_rep = workload_size;

  std::vector<double> rep_p50s;
  std::vector<double> rep_p99s;
  std::vector<double> rep_p999s;
  std::vector<double> rep_means;
  double sum_cpu_wall = 0.0;
  double min_v = runs[0].rtt_half_ns.min;
  double max_v = runs[0].rtt_half_ns.max;

  for (size_t i = 0; i < runs.size(); ++i) {
    rep_p50s.push_back(runs[i].rtt_half_ns.p50);
    rep_p99s.push_back(runs[i].rtt_half_ns.p99);
    rep_p999s.push_back(runs[i].rtt_half_ns.p999);
    rep_means.push_back(runs[i].rtt_half_ns.mean);
    sum_cpu_wall += runs[i].cpu_to_wall_ratio;
    min_v = std::min(min_v, runs[i].rtt_half_ns.min);
    max_v = std::max(max_v, runs[i].rtt_half_ns.max);
    r.raw_ns_per_op.push_back(runs[i].rtt_half_ns.p50);
    r.raw_rep_index.push_back(i);
    r.checksum = runs[i].checksum;
  }

  const auto p50_stats = lpl::compute_stats(rep_p50s);
  r.ns_per_op.count = runs.size() * workload_size;
  r.ns_per_op.min = min_v;
  r.ns_per_op.p50 = p50_stats.p50;
  r.ns_per_op.p90 = lpl::compute_stats(rep_p99s).min;
  r.ns_per_op.p95 = lpl::compute_stats(rep_p99s).p50;
  r.ns_per_op.p99 = lpl::compute_stats(rep_p99s).p50;
  r.ns_per_op.p999 = lpl::compute_stats(rep_p999s).p50;
  r.ns_per_op.max = max_v;
  r.ns_per_op.mean = lpl::compute_stats(rep_means).mean;
  r.ns_per_op.stddev = p50_stats.stddev;
  r.ns_per_op.iqr = p50_stats.iqr;
  r.ns_per_op.cv_pct = p50_stats.cv_pct;

  r.rep_p50_min = p50_stats.min;
  r.rep_p50_max = p50_stats.max;
  r.rep_p50_cv_pct = p50_stats.cv_pct;
  r.throughput_mops = (r.ns_per_op.p50 > 0.0) ? (1000.0 / r.ns_per_op.p50) : 0.0;
  r.cpu_to_wall_ratio = sum_cpu_wall / static_cast<double>(runs.size());
  r.perf_backend = "rusage_cpu_wall";
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const CliOptions opt = parse_cli(argc, argv);
  std::filesystem::create_directories(opt.out_dir);

  const auto env = lpl::collect_environment_info();
  const auto aff = lpl::pin_current_thread(opt.pin_core);

  std::cout << "=== Latency Performance Lab: Microbenchmark Suite ===\n";
  std::cout << lpl::environment_to_markdown(env);
  std::cout << "Affinity: " << aff.note << "\n";
  std::cout << "Config: warmup=" << opt.warmup << ", reps=" << opt.reps
            << ", samples/rep=" << opt.samples << "\n\n";

  {
    std::ofstream env_out(opt.out_dir + "/environment.json");
    env_out << lpl::environment_to_json(env);
  }

  std::vector<lpl::BenchmarkResult> results;

  // 1. AoS vs SoA
  if (want_suite(opt, "aos_soa")) {
    const size_t n = opt.quick ? 16384 : 131072;  // 131072 * 64B = 8 MiB (> L2 cache)
    const auto ds = lpl::make_aos_soa_dataset(n, 42);
    lpl::BenchmarkConfig cfg{opt.warmup, opt.reps, opt.samples, n, opt.pin_core};

    auto r_seq_aos = lpl::run_benchmark("aos_soa", "sequential_scan", "aos_64B", n, cfg,
                                        [&]() { return lpl::scan_sequential_aos(ds.aos); });
    auto r_seq_soa = lpl::run_benchmark("aos_soa", "sequential_scan", "soa_split", n, cfg,
                                        [&]() { return lpl::scan_sequential_soa(ds.soa); });
    assert_equal_checksum("aos_soa/sequential_scan", r_seq_aos.checksum, r_seq_soa.checksum);

    auto r_rnd1_aos = lpl::run_benchmark(
        "aos_soa", "random_single_field", "aos_64B", n, cfg,
        [&]() { return lpl::scan_random_single_field_aos(ds.aos, ds.random_indices); });
    auto r_rnd1_soa = lpl::run_benchmark(
        "aos_soa", "random_single_field", "soa_split", n, cfg,
        [&]() { return lpl::scan_random_single_field_soa(ds.soa, ds.random_indices); });
    assert_equal_checksum("aos_soa/random_single_field", r_rnd1_aos.checksum, r_rnd1_soa.checksum);

    auto r_rnd4_aos = lpl::run_benchmark(
        "aos_soa", "random_multi_field", "aos_64B", n, cfg,
        [&]() { return lpl::scan_random_multi_field_aos(ds.aos, ds.random_indices); });
    auto r_rnd4_soa = lpl::run_benchmark(
        "aos_soa", "random_multi_field", "soa_split", n, cfg,
        [&]() { return lpl::scan_random_multi_field_soa(ds.soa, ds.random_indices); });
    assert_equal_checksum("aos_soa/random_multi_field", r_rnd4_aos.checksum, r_rnd4_soa.checksum);

    results.push_back(std::move(r_seq_aos));
    results.push_back(std::move(r_seq_soa));
    results.push_back(std::move(r_rnd1_aos));
    results.push_back(std::move(r_rnd1_soa));
    results.push_back(std::move(r_rnd4_aos));
    results.push_back(std::move(r_rnd4_soa));
  }

  // 2. Order-book data structures: std::map vs sorted vector vs unordered_map vs flat hash
  if (want_suite(opt, "order_book")) {
    const size_t num_events = opt.quick ? 5000 : 25000;
    const std::vector<size_t> depths = opt.quick ? std::vector<size_t>{64}
                                                 : std::vector<size_t>{64, 512};
    for (size_t depth : depths) {
      const auto events = lpl::generate_order_book_workload(num_events, depth, 1337 + depth);
      const std::string bname = "book_depth_" + std::to_string(depth);
      lpl::BenchmarkConfig cfg{opt.warmup, opt.reps, opt.samples, num_events, opt.pin_core};

      lpl::MapPriceBook map_book;
      lpl::SortedVectorPriceBook vec_book;
      vec_book.reserve(depth * 2);
      lpl::UnorderedMapPriceBook umap_book;
      umap_book.reserve(depth * 2);
      lpl::FlatHashPriceBook flat_book(depth * 4);

      auto r_map = lpl::run_benchmark("order_book", bname, "std_map", depth, cfg,
                                      [&]() { return lpl::replay_price_book(map_book, events); });
      auto r_vec = lpl::run_benchmark("order_book", bname, "sorted_vector", depth, cfg,
                                      [&]() { return lpl::replay_price_book(vec_book, events); });
      auto r_umap = lpl::run_benchmark("order_book", bname, "std_unordered_map", depth, cfg,
                                       [&]() { return lpl::replay_price_book(umap_book, events); });
      auto r_flat = lpl::run_benchmark("order_book", bname, "flat_hash_map", depth, cfg,
                                       [&]() { return lpl::replay_price_book(flat_book, events); });

      assert_equal_checksum(bname + "/map_vs_vec", r_map.checksum, r_vec.checksum);
      assert_equal_checksum(bname + "/map_vs_umap", r_map.checksum, r_umap.checksum);
      assert_equal_checksum(bname + "/map_vs_flat", r_map.checksum, r_flat.checksum);

      results.push_back(std::move(r_map));
      results.push_back(std::move(r_vec));
      results.push_back(std::move(r_umap));
      results.push_back(std::move(r_flat));
    }
  }

  // 3. Allocators: heap new/delete vs ObjectPool vs MonotonicArena
  if (want_suite(opt, "allocator")) {
    const size_t num_ops = opt.quick ? 4096 : 16384;
    const size_t max_live = opt.quick ? 1024 : 4096;
    const auto trace = lpl::generate_alloc_churn_trace(num_ops, max_live, 2026);
    lpl::BenchmarkConfig cfg{opt.warmup, opt.reps, opt.samples, num_ops, opt.pin_core};

    lpl::ObjectPool<lpl::TrackedOrder> pool(max_live + 16);
    lpl::MonotonicArena arena((num_ops + 16) * sizeof(lpl::TrackedOrder));

    lpl::TrackedOrder::reset_counters();
    auto r_heap = lpl::run_benchmark("allocator", "order_churn", "heap_new_delete", num_ops, cfg,
                                     [&]() { return lpl::run_heap_churn(trace, max_live); });
    if (lpl::TrackedOrder::live_count != 0) {
      std::cerr << "MEMORY LIFECYCLE BUG in heap_new_delete\n";
      return 2;
    }

    auto r_pool = lpl::run_benchmark("allocator", "order_churn", "object_pool", num_ops, cfg,
                                     [&]() { return lpl::run_pool_churn(pool, trace, max_live); });
    if (lpl::TrackedOrder::live_count != 0) {
      std::cerr << "MEMORY LIFECYCLE BUG in object_pool\n";
      return 2;
    }

    auto r_arena =
        lpl::run_benchmark("allocator", "order_churn", "monotonic_arena", num_ops, cfg,
                           [&]() { return lpl::run_arena_churn(arena, trace, max_live); });
    if (lpl::TrackedOrder::live_count != 0) {
      std::cerr << "MEMORY LIFECYCLE BUG in monotonic_arena\n";
      return 2;
    }

    assert_equal_checksum("allocator/heap_vs_pool", r_heap.checksum, r_pool.checksum);
    assert_equal_checksum("allocator/heap_vs_arena", r_heap.checksum, r_arena.checksum);

    results.push_back(std::move(r_heap));
    results.push_back(std::move(r_pool));
    results.push_back(std::move(r_arena));
  }

  // 4. Dispatch: virtual vs function pointers vs variant vs templates
  if (want_suite(opt, "dispatch")) {
    const size_t n = opt.quick ? 16384 : 65536;
    lpl::BenchmarkConfig cfg{opt.warmup, opt.reps, opt.samples, n, opt.pin_core};

    // 4a. Monomorphic
    {
      const auto events = lpl::generate_dispatch_events(n, false, 9001);
      lpl::VirtualAlphaA va({7, 3, 0});
      std::vector<lpl::VirtualHandler*> vptrs(n, &va);

      lpl::HandlerState fn_state{7, 3, 0};
      std::vector<lpl::FnPtrHandler> fptrs(n, lpl::FnPtrHandler{&lpl::compute_alpha_a, &fn_state});

      lpl::StaticAlphaA sa{{7, 3, 0}};

      auto r_virt = lpl::run_benchmark("dispatch", "monomorphic", "virtual_dispatch", n, cfg,
                                       [&]() { return lpl::run_virtual_dispatch(events, vptrs); });
      auto r_fn = lpl::run_benchmark("dispatch", "monomorphic", "function_pointer", n, cfg,
                                     [&]() { return lpl::run_fnptr_dispatch(events, fptrs); });
      auto r_tmpl =
          lpl::run_benchmark("dispatch", "monomorphic", "template_static", n, cfg,
                             [&]() { return lpl::run_template_monomorphic(events, sa); });

      assert_equal_checksum("dispatch/mono_virt_vs_fn", r_virt.checksum, r_fn.checksum);
      assert_equal_checksum("dispatch/mono_virt_vs_tmpl", r_virt.checksum, r_tmpl.checksum);

      results.push_back(std::move(r_virt));
      results.push_back(std::move(r_fn));
      results.push_back(std::move(r_tmpl));
    }

    // 4b. Heterogeneous (3 concrete handler types, shuffled uniformly)
    {
      const auto events = lpl::generate_dispatch_events(n, true, 9002);
      lpl::VirtualAlphaA va({7, 3, 0});
      lpl::VirtualAlphaB vb({11, 5, 0});
      lpl::VirtualAlphaC vc({13, 2, 0});
      std::vector<lpl::VirtualHandler*> vptrs(n);
      for (size_t i = 0; i < n; ++i) {
        vptrs[i] = (events[i].handler_kind == 0)
                       ? static_cast<lpl::VirtualHandler*>(&va)
                       : (events[i].handler_kind == 1 ? static_cast<lpl::VirtualHandler*>(&vb)
                                                      : static_cast<lpl::VirtualHandler*>(&vc));
      }

      lpl::HandlerState st_a{7, 3, 0};
      lpl::HandlerState st_b{11, 5, 0};
      lpl::HandlerState st_c{13, 2, 0};
      std::vector<lpl::FnPtrHandler> fptrs(n);
      for (size_t i = 0; i < n; ++i) {
        if (events[i].handler_kind == 0) {
          fptrs[i] = {&lpl::compute_alpha_a, &st_a};
        } else if (events[i].handler_kind == 1) {
          fptrs[i] = {&lpl::compute_alpha_b, &st_b};
        } else {
          fptrs[i] = {&lpl::compute_alpha_c, &st_c};
        }
      }

      std::vector<lpl::HandlerVariant> variants = {
          lpl::StaticAlphaA{{7, 3, 0}},
          lpl::StaticAlphaB{{11, 5, 0}},
          lpl::StaticAlphaC{{13, 2, 0}},
      };

      lpl::StaticAlphaA sa{{7, 3, 0}};
      lpl::StaticAlphaB sb{{11, 5, 0}};
      lpl::StaticAlphaC sc{{13, 2, 0}};

      auto r_virt = lpl::run_benchmark("dispatch", "heterogeneous", "virtual_dispatch", n, cfg,
                                       [&]() { return lpl::run_virtual_dispatch(events, vptrs); });
      auto r_fn = lpl::run_benchmark("dispatch", "heterogeneous", "function_pointer", n, cfg,
                                     [&]() { return lpl::run_fnptr_dispatch(events, fptrs); });
      auto r_var = lpl::run_benchmark("dispatch", "heterogeneous", "std_variant_visit", n, cfg,
                                      [&]() { return lpl::run_variant_dispatch(events, variants); });
      auto r_sw = lpl::run_benchmark("dispatch", "heterogeneous", "static_switch", n, cfg, [&]() {
        return lpl::run_switch_static_dispatch(events, sa, sb, sc);
      });

      assert_equal_checksum("dispatch/het_virt_vs_fn", r_virt.checksum, r_fn.checksum);
      assert_equal_checksum("dispatch/het_virt_vs_var", r_virt.checksum, r_var.checksum);
      assert_equal_checksum("dispatch/het_virt_vs_sw", r_virt.checksum, r_sw.checksum);

      results.push_back(std::move(r_virt));
      results.push_back(std::move(r_fn));
      results.push_back(std::move(r_var));
      results.push_back(std::move(r_sw));
    }
  }

  // 5. Branch predictability & cache-line false sharing
  if (want_suite(opt, "branch_sharing")) {
    const size_t n = opt.quick ? 16384 : 65536;
    const auto ds = lpl::make_branch_dataset(n, 777);
    lpl::BenchmarkConfig cfg{opt.warmup, opt.reps, opt.samples, n, opt.pin_core};

    auto r_nat_s = lpl::run_benchmark("branch", "filter_sorted", "natural_if", n, cfg, [&]() {
      return lpl::branch_filter_natural_if(ds.sorted, ds.threshold);
    });
    auto r_nat_u = lpl::run_benchmark("branch", "filter_unsorted", "natural_if", n, cfg, [&]() {
      return lpl::branch_filter_natural_if(ds.unsorted, ds.threshold);
    });
    auto r_fb_s = lpl::run_benchmark("branch", "filter_sorted", "forced_branch", n, cfg, [&]() {
      return lpl::branch_filter_forced_branch(ds.sorted, ds.threshold);
    });
    auto r_fb_u = lpl::run_benchmark("branch", "filter_unsorted", "forced_branch", n, cfg, [&]() {
      return lpl::branch_filter_forced_branch(ds.unsorted, ds.threshold);
    });
    auto r_bl_s = lpl::run_benchmark("branch", "filter_sorted", "branchless_mask", n, cfg, [&]() {
      return lpl::branch_filter_branchless(ds.sorted, ds.threshold);
    });
    auto r_bl_u = lpl::run_benchmark("branch", "filter_unsorted", "branchless_mask", n, cfg, [&]() {
      return lpl::branch_filter_branchless(ds.unsorted, ds.threshold);
    });

    assert_equal_checksum("branch/nat_s_vs_u", r_nat_s.checksum, r_nat_u.checksum);
    assert_equal_checksum("branch/nat_vs_fb_s", r_nat_s.checksum, r_fb_s.checksum);
    assert_equal_checksum("branch/nat_vs_fb_u", r_nat_s.checksum, r_fb_u.checksum);
    assert_equal_checksum("branch/nat_vs_bl_s", r_nat_s.checksum, r_bl_s.checksum);
    assert_equal_checksum("branch/nat_vs_bl_u", r_nat_s.checksum, r_bl_u.checksum);

    results.push_back(std::move(r_nat_s));
    results.push_back(std::move(r_nat_u));
    results.push_back(std::move(r_fb_s));
    results.push_back(std::move(r_fb_u));
    results.push_back(std::move(r_bl_s));
    results.push_back(std::move(r_bl_u));

    // False sharing across 4 threads: 8B packed vs 64B vs 128B padded
    const size_t threads = 4;
    const size_t iters = opt.quick ? 200'000 : 1'000'000;
    lpl::BenchmarkConfig fs_cfg{1, opt.reps, opt.quick ? size_t{2} : size_t{5}, threads * iters,
                                -1};

    auto r_fs8 = lpl::run_benchmark("false_sharing", "4_threads_atomic", "align_8B_packed",
                                    threads * iters, fs_cfg, [&]() {
                                      return lpl::run_false_sharing_counters<8>(threads, iters,
                                                                                false);
                                    });
    auto r_fs64 = lpl::run_benchmark("false_sharing", "4_threads_atomic", "align_64B_padded",
                                     threads * iters, fs_cfg, [&]() {
                                       return lpl::run_false_sharing_counters<64>(threads, iters,
                                                                                  false);
                                     });
    auto r_fs128 = lpl::run_benchmark("false_sharing", "4_threads_atomic", "align_128B_padded",
                                      threads * iters, fs_cfg, [&]() {
                                        return lpl::run_false_sharing_counters<128>(threads, iters,
                                                                                    false);
                                      });

    assert_equal_checksum("false_sharing/8_vs_64", r_fs8.checksum, r_fs64.checksum);
    assert_equal_checksum("false_sharing/8_vs_128", r_fs8.checksum, r_fs128.checksum);

    results.push_back(std::move(r_fs8));
    results.push_back(std::move(r_fs64));
    results.push_back(std::move(r_fs128));
  }

  // 6. Busy-spin vs hybrid vs blocking wakeup & SPSC queue handoff
  if (want_suite(opt, "spsc")) {
    const size_t ping_rounds = opt.quick ? 500 : 3000;
    const size_t handoff_reps = opt.quick ? 2 : std::min<size_t>(opt.reps, 5);

    auto run_handoff_suite = [&](const std::string& bname, uint64_t pacing_ns, size_t rounds) {
      std::vector<lpl::HandoffMetrics> spin_runs;
      std::vector<lpl::HandoffMetrics> hybrid_runs;
      std::vector<lpl::HandoffMetrics> block_runs;
      for (size_t r = 0; r < handoff_reps; ++r) {
        spin_runs.push_back(lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kBusySpin, rounds,
                                                           200, pacing_ns, true));
        hybrid_runs.push_back(lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kHybridSpinYield,
                                                             rounds, 200, pacing_ns, true));
        block_runs.push_back(lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kBlockingCondVar,
                                                            rounds, 200, pacing_ns, true));
      }
      assert_equal_checksum(bname + "/spin_vs_hybrid", spin_runs[0].checksum,
                            hybrid_runs[0].checksum);
      assert_equal_checksum(bname + "/spin_vs_block", spin_runs[0].checksum,
                            block_runs[0].checksum);

      results.push_back(make_handoff_result(bname, "busy_spin_spsc", rounds, handoff_reps,
                                            spin_runs));
      results.push_back(make_handoff_result(bname, "hybrid_spin_yield", rounds, handoff_reps,
                                            hybrid_runs));
      results.push_back(make_handoff_result(bname, "blocking_condvar", rounds, handoff_reps,
                                            block_runs));
    };

    // Tight ping-pong (zero idle time) + paced ping-pong (50us idle interval -> shows CPU cost!)
    run_handoff_suite("ping_pong_tight", 0, ping_rounds);
    run_handoff_suite("ping_pong_paced_50us", 50'000, opt.quick ? 100 : 400);

    // SPSC burst handoff: pinned vs unpinned core placement comparison
    const size_t burst_n = opt.quick ? 100'000 : 500'000;
    lpl::BenchmarkConfig burst_cfg{1, opt.reps, opt.quick ? size_t{2} : size_t{5}, burst_n, -1};
    auto r_burst_pin =
        lpl::run_benchmark("spsc", "burst_handoff", "spsc_pinned", burst_n, burst_cfg,
                           [&]() { return lpl::run_spsc_burst_handoff(burst_n, 4096, true); });
    auto r_burst_unpin =
        lpl::run_benchmark("spsc", "burst_handoff", "spsc_unpinned", burst_n, burst_cfg,
                           [&]() { return lpl::run_spsc_burst_handoff(burst_n, 4096, false); });
    assert_equal_checksum("spsc/burst_pin_vs_unpin", r_burst_pin.checksum, r_burst_unpin.checksum);
    results.push_back(std::move(r_burst_pin));
    results.push_back(std::move(r_burst_unpin));
  }

  std::cout << lpl::format_results_table(results) << "\n";

  const std::string sum_csv = opt.out_dir + "/microbenchmarks_summary.csv";
  const std::string raw_csv = opt.out_dir + "/microbenchmarks_raw.csv";
  if (!lpl::write_summary_csv(sum_csv, results) || !lpl::write_raw_samples_csv(raw_csv, results)) {
    std::cerr << "Failed to write CSV output to " << opt.out_dir << "\n";
    return 1;
  }

  std::cout << "Wrote: " << sum_csv << "\n";
  std::cout << "Wrote: " << raw_csv << "\n";
  return 0;
}
