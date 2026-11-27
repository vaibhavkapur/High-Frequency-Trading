#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "numa_engine/pipeline.hpp"
#include "numa_engine/topology.hpp"

int main(int argc, char** argv) {
    using namespace numa_engine;

    ComparisonRunConfig cfg;
    cfg.num_events = 200000;
    cfg.warmup_events = 10000;
    cfg.queue_capacity = 4096;
    cfg.num_symbols = 16;
    cfg.seed = 42;
    cfg.repetitions = 5;
    cfg.allow_single_node_fallback = false;

    std::string out_json = "reports/raw_benchmark_results.json";
    std::string out_csv = "reports/benchmark_summary.csv";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) {
            cfg.num_events = std::stoull(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            cfg.warmup_events = std::stoull(argv[++i]);
        } else if (arg == "--capacity" && i + 1 < argc) {
            cfg.queue_capacity = std::stoull(argv[++i]);
        } else if (arg == "--reps" && i + 1 < argc) {
            cfg.repetitions = std::stoull(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = std::stoull(argv[++i]);
        } else if (arg == "--json" && i + 1 < argc) {
            out_json = argv[++i];
        } else if (arg == "--csv" && i + 1 < argc) {
            out_csv = argv[++i];
        } else if (arg == "--allow-fallback") {
            cfg.allow_single_node_fallback = true;
        }
    }

    const TopologyInfo topo = detect_topology();
    auto res = run_comparison_suite(cfg, topo);
    if (!res.ok()) {
        std::cerr << "Benchmark failed [" << to_string(res.error().code) << "]: "
                  << res.error().message << "\n";
        return 1;
    }

    const auto& report = res.value();

    {
        std::ofstream jfs(out_json);
        if (!jfs.is_open()) {
            std::cerr << "Failed to open output JSON path: " << out_json << "\n";
            return 1;
        }
        jfs << report.to_json() << "\n";
    }

    {
        std::ofstream cfs(out_csv);
        if (!cfs.is_open()) {
            std::cerr << "Failed to open output CSV path: " << out_csv << "\n";
            return 1;
        }
        cfs << "scenario_name,pipeline_kind,requested_policy,status,repetitions,"
            << "mean_throughput_eps,min_throughput_eps,max_throughput_eps,"
            << "median_p50_ns,median_p99_ns,median_p99_9_ns,total_lost_events,digest_matches\n";
        for (const auto& s : report.scenarios) {
            cfs << s.scenario_name << ','
                << to_string(s.pipeline_kind) << ','
                << to_string(s.requested_policy) << ','
                << to_string(s.status) << ','
                << s.repetitions << ','
                << std::fixed << std::setprecision(2) << s.mean_throughput_eps << ','
                << s.min_throughput_eps << ','
                << s.max_throughput_eps << ','
                << std::setprecision(1) << s.median_p50_ns << ','
                << s.median_p99_ns << ','
                << s.median_p99_9_ns << ','
                << s.total_lost_events << ','
                << (s.all_digests_match_baseline ? "true" : "false") << '\n';
        }
    }

    std::cout << "Benchmark completed (report_id=" << report.report_id << ")\n";
    std::cout << "  JSON: " << out_json << "\n";
    std::cout << "  CSV : " << out_csv << "\n";
    std::cout << "  Functional equivalence: " << (report.functional_equivalence_passed ? "PASS" : "FAIL") << "\n";
    std::cout << "  Zero lost events      : " << (report.zero_lost_events ? "PASS" : "FAIL") << "\n";

    return (report.functional_equivalence_passed && report.zero_lost_events) ? 0 : 1;
}
