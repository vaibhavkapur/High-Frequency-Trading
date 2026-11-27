#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "numa_engine/event_generator.hpp"
#include "numa_engine/pipeline.hpp"
#include "numa_engine/topology.hpp"
#include "numa_engine/types.hpp"

namespace {

void print_usage() {
    std::cout
        << "NUMA-Aware Trading Engine CLI (v" << NUMA_ENGINE_VERSION << ", schema " << numa_engine::kSchemaVersion << ")\n\n"
        << "Usage:\n"
        << "  numa_trading_engine --mode <topology|run|compare|validate-fixture> [options]\n\n"
        << "Options:\n"
        << "  --mode <mode>            Operation mode: topology, run, compare, validate-fixture\n"
        << "  --config <path>          Path to JSON pipeline config (for --mode run)\n"
        << "  --fixture <path>         Path to CSV event fixture (for run or validate-fixture)\n"
        << "  --pipeline <kind>        baseline_mutex | lock_free_spsc (default: lock_free_spsc)\n"
        << "  --policy <policy>        default | first_touch_local | local | remote | interleaved\n"
        << "  --events <N>             Number of measured synthetic events (default: 100000)\n"
        << "  --warmup <N>             Number of warmup events (default: 5000)\n"
        << "  --capacity <N>           Queue capacity (power of two for SPSC; default: 4096)\n"
        << "  --symbols <N>            Number of synthetic symbols (default: 16)\n"
        << "  --seed <N>               Fixed deterministic PRNG seed (default: 42)\n"
        << "  --repetitions <N>        Repeated runs per placement in compare mode (default: 5)\n"
        << "  --producer-cpu <id>      Producer CPU ID (-1 unpinned)\n"
        << "  --consumer-cpu <id>      Consumer CPU ID (-1 unpinned)\n"
        << "  --memory-node <id>       Preferred NUMA memory node (default: 0)\n"
        << "  --consumer-work <iters>  Synthetic per-event consumer work iterations (default: 0)\n"
        << "  --allow-fallback         Allow single-node fallback instead of marking unavailable\n"
        << "  --strict-pinning         Fail if OS cannot enforce hard CPU pinning\n"
        << "  --output <path>          Write JSON output to file in addition to summary\n"
        << "  --help                   Show this help text\n";
}

void print_comparison_table(const numa_engine::ComparisonReport& rep) {
    std::cout << "\n=== NUMA-Aware Trading Engine Comparison Summary ===\n";
    std::cout << "Report ID : " << rep.report_id << "\n";
    std::cout << "Host      : " << rep.topology.os_name << " " << rep.topology.os_release
              << " (" << rep.topology.arch << ") - " << rep.topology.cpu_model << "\n";
    std::cout << "Topology  : " << rep.topology.logical_cpus << " logical CPUs, "
              << rep.topology.numa_node_count() << " NUMA node(s), multi_numa="
              << (rep.topology.is_multi_numa ? "true" : "false") << "\n";
    std::cout << "Clock     : " << rep.clock_source << " | Build: " << rep.build_type
              << " (" << rep.compiler << ")\n";
    std::cout << "Workload  : " << rep.config.num_events << " measured + "
              << rep.config.warmup_events << " warmup events, capacity="
              << rep.config.queue_capacity << ", seed=" << rep.config.seed
              << ", reps=" << rep.config.repetitions << "\n\n";

    std::cout << std::left
              << std::setw(26) << "Scenario"
              << std::setw(24) << "Status"
              << std::right
              << std::setw(6) << "Reps"
              << std::setw(14) << "Mean eps"
              << std::setw(12) << "p50 (ns)"
              << std::setw(12) << "p99 (ns)"
              << std::setw(13) << "p99.9 (ns)"
              << std::setw(8) << "Lost"
              << std::setw(8) << "Equiv"
              << "\n";
    std::cout << std::string(123, '-') << "\n";

    for (const auto& s : rep.scenarios) {
        std::cout << std::left
                  << std::setw(26) << s.scenario_name
                  << std::setw(24) << numa_engine::to_string(s.status)
                  << std::right
                  << std::setw(6) << s.repetitions;
        if (s.repetitions > 0) {
            std::cout << std::setw(14) << std::fixed << std::setprecision(0) << s.mean_throughput_eps
                      << std::setw(12) << std::fixed << std::setprecision(0) << s.median_p50_ns
                      << std::setw(12) << std::fixed << std::setprecision(0) << s.median_p99_ns
                      << std::setw(13) << std::fixed << std::setprecision(0) << s.median_p99_9_ns
                      << std::setw(8) << s.total_lost_events
                      << std::setw(8) << (s.all_digests_match_baseline ? "PASS" : "FAIL");
        } else {
            std::cout << std::setw(14) << "N/A"
                      << std::setw(12) << "N/A"
                      << std::setw(12) << "N/A"
                      << std::setw(13) << "N/A"
                      << std::setw(8) << 0
                      << std::setw(8) << "N/A";
        }
        std::cout << "\n";
    }
    std::cout << "\nBaseline state digest : 0x" << std::hex << rep.baseline_digest << std::dec << "\n";
    std::cout << "Functional equivalence: " << (rep.functional_equivalence_passed ? "PASS" : "FAIL") << "\n";
    std::cout << "Zero lost events      : " << (rep.zero_lost_events ? "PASS" : "FAIL") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace numa_engine;

    std::string mode = "compare";
    std::string config_path;
    std::string fixture_path;
    std::string output_path;
    std::string pipeline_str = "lock_free_spsc";
    std::string policy_str = "first_touch_local";

    std::size_t num_events = 100000;
    std::size_t warmup_events = 5000;
    std::size_t capacity = 4096;
    std::uint16_t symbols = 16;
    std::uint64_t seed = 42;
    std::size_t repetitions = 5;
    int prod_cpu = -1;
    int cons_cpu = -1;
    int mem_node = 0;
    std::uint32_t consumer_work = 0;
    bool allow_fallback = false;
    bool strict_pinning = false;

    auto require_arg = [&](int& i, const std::string& flag) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "Error: missing value for " << flag << "\n";
            std::exit(2);
        }
        return std::string(argv[++i]);
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--mode") {
            mode = require_arg(i, arg);
        } else if (arg == "--config") {
            config_path = require_arg(i, arg);
        } else if (arg == "--fixture") {
            fixture_path = require_arg(i, arg);
        } else if (arg == "--pipeline") {
            pipeline_str = require_arg(i, arg);
        } else if (arg == "--policy") {
            policy_str = require_arg(i, arg);
        } else if (arg == "--events") {
            num_events = std::stoull(require_arg(i, arg));
        } else if (arg == "--warmup") {
            warmup_events = std::stoull(require_arg(i, arg));
        } else if (arg == "--capacity") {
            capacity = std::stoull(require_arg(i, arg));
        } else if (arg == "--symbols") {
            symbols = static_cast<std::uint16_t>(std::stoul(require_arg(i, arg)));
        } else if (arg == "--seed") {
            seed = std::stoull(require_arg(i, arg));
        } else if (arg == "--repetitions") {
            repetitions = std::stoull(require_arg(i, arg));
        } else if (arg == "--producer-cpu") {
            prod_cpu = std::stoi(require_arg(i, arg));
        } else if (arg == "--consumer-cpu") {
            cons_cpu = std::stoi(require_arg(i, arg));
        } else if (arg == "--memory-node") {
            mem_node = std::stoi(require_arg(i, arg));
        } else if (arg == "--consumer-work") {
            consumer_work = static_cast<std::uint32_t>(std::stoul(require_arg(i, arg)));
        } else if (arg == "--allow-fallback") {
            allow_fallback = true;
        } else if (arg == "--strict-pinning") {
            strict_pinning = true;
        } else if (arg == "--output") {
            output_path = require_arg(i, arg);
        } else {
            std::cerr << "Error: unknown argument '" << arg << "'\n";
            print_usage();
            return 2;
        }
    }

    const TopologyInfo topo = detect_topology();

    if (mode == "topology") {
        const std::string json = topo.to_json();
        std::cout << json << "\n";
        if (!output_path.empty()) {
            std::ofstream ofs(output_path);
            ofs << json << "\n";
        }
        return 0;
    }

    if (mode == "validate-fixture") {
        if (fixture_path.empty()) {
            std::cerr << "Error: --fixture <path> is required for --mode validate-fixture\n";
            return 2;
        }
        auto res = load_events_csv(fixture_path);
        if (!res.ok()) {
            std::cerr << "VALIDATION_FAILED [" << to_string(res.error().code) << "] "
                      << "field=" << res.error().field << " line=" << res.error().line
                      << ": " << res.error().message << "\n";
            return 1;
        }
        std::cout << "VALIDATION_OK: " << res.value().size() << " events validated from "
                  << fixture_path << "\n";
        return 0;
    }

    if (mode == "run") {
        PipelineConfig pcfg;
        if (!config_path.empty()) {
            auto cfg_res = load_pipeline_config_json(config_path);
            if (!cfg_res.ok()) {
                std::cerr << "CONFIG_ERROR [" << to_string(cfg_res.error().code) << "] "
                          << cfg_res.error().message << "\n";
                return 1;
            }
            pcfg = cfg_res.value();
        } else {
            auto kind = parse_pipeline_kind(pipeline_str);
            auto pol = parse_memory_policy(policy_str);
            if (!kind) {
                std::cerr << "Error: invalid --pipeline '" << pipeline_str << "'\n";
                return 2;
            }
            if (!pol) {
                std::cerr << "Error: invalid --policy '" << policy_str << "'\n";
                return 2;
            }
            pcfg.scenario_name = pipeline_str + "_" + policy_str;
            pcfg.pipeline_kind = *kind;
            pcfg.memory_policy = *pol;
            pcfg.queue_capacity = capacity;
            pcfg.producer_cpu = prod_cpu;
            pcfg.consumer_cpu = cons_cpu;
            pcfg.preferred_memory_node = mem_node;
            pcfg.allow_single_node_fallback = allow_fallback;
            pcfg.require_hard_pinning = strict_pinning;
            pcfg.consumer_work_iters = consumer_work;
            pcfg.seed = seed;
            pcfg.warmup_events = warmup_events;
        }

        std::vector<MarketEvent> events;
        if (!fixture_path.empty()) {
            auto ev_res = load_events_csv(fixture_path);
            if (!ev_res.ok()) {
                std::cerr << "FIXTURE_ERROR [" << to_string(ev_res.error().code) << "] "
                          << ev_res.error().message << "\n";
                return 1;
            }
            events = std::move(ev_res.value());
            pcfg.input_source = fixture_path;
        } else {
            GeneratorConfig gcfg;
            gcfg.seed = pcfg.seed;
            gcfg.num_events = num_events;
            gcfg.warmup_events = pcfg.warmup_events;
            gcfg.num_symbols = symbols;
            auto ev_res = generate_synthetic_events(gcfg);
            if (!ev_res.ok()) {
                std::cerr << "GENERATOR_ERROR [" << to_string(ev_res.error().code) << "] "
                          << ev_res.error().message << "\n";
                return 1;
            }
            events = std::move(ev_res.value());
        }

        auto run_res = run_pipeline(pcfg, events, topo);
        if (!run_res.ok()) {
            std::cerr << "RUN_ERROR [" << to_string(run_res.error().code) << "] "
                      << run_res.error().message << "\n";
            return 1;
        }

        const std::string json = run_res.value().to_json();
        std::cout << json << "\n";
        if (!output_path.empty()) {
            std::ofstream ofs(output_path);
            ofs << json << "\n";
        }
        return 0;
    }

    if (mode == "compare") {
        ComparisonRunConfig ccfg;
        ccfg.num_events = num_events;
        ccfg.warmup_events = warmup_events;
        ccfg.queue_capacity = capacity;
        ccfg.num_symbols = symbols;
        ccfg.seed = seed;
        ccfg.repetitions = repetitions;
        ccfg.allow_single_node_fallback = allow_fallback;
        ccfg.consumer_work_iters = consumer_work;

        auto cmp_res = run_comparison_suite(ccfg, topo);
        if (!cmp_res.ok()) {
            std::cerr << "COMPARE_ERROR [" << to_string(cmp_res.error().code) << "] "
                      << cmp_res.error().message << "\n";
            return 1;
        }

        print_comparison_table(cmp_res.value());
        if (!output_path.empty()) {
            std::ofstream ofs(output_path);
            ofs << cmp_res.value().to_json() << "\n";
            std::cout << "\nRaw JSON report written to: " << output_path << "\n";
        }
        return (cmp_res.value().functional_equivalence_passed && cmp_res.value().zero_lost_events) ? 0 : 1;
    }

    std::cerr << "Error: unsupported --mode '" << mode << "'\n";
    print_usage();
    return 2;
}
