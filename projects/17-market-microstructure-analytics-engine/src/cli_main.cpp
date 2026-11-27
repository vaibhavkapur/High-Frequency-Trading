#include "mmae/feature_engine.hpp"
#include "mmae/io_adapter.hpp"
#include "mmae/types.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cerr
        << "Usage: mmae_cli --input <events.csv> --output-features <features.csv> "
        << "--output-diagnostics <diagnostics.json> [options]\n"
        << "Options:\n"
        << "  --config <config.json>           Load EngineConfig from JSON file\n"
        << "  --variant <baseline|optimized>   Book implementation (default: optimized)\n"
        << "  --tick-size <float>              Tick size in quote currency (default: 0.01)\n"
        << "  --stale-threshold-ns <int64>     Stale quote threshold in ns (default: 1000000000)\n"
        << "  --flow-window-ns <int64>         Rolling causal flow window in ns (default: 1000000000)\n"
        << "  --depth-k <int>                  Top-K depth levels for OBI (default: 5)\n"
        << "  --max-levels <int>               Bounded max levels per side (default: 1024)\n"
        << "  --max-orders <int>               Bounded max active L3 orders (default: 65536)\n"
        << "  --run-id <string>                Stable run identifier for diagnostics\n"
        << "  --strict-fail-on-invalid         Exit non-zero if any input row fails validation\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_features_path;
    std::string output_diag_path;
    std::string config_path;
    std::string run_id = "local-run";
    bool strict_fail = false;

    mmae::EngineConfig cfg{};

    // First pass: check if --config is specified so CLI flags can override it.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }

    if (!config_path.empty()) {
        std::string cfg_err;
        if (!mmae::load_config_json(config_path, cfg, cfg_err)) {
            std::cerr << "[ERROR] Config load failed: " << cfg_err << "\n";
            return 2;
        }
    }

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto require_val = [&](const std::string& flag) -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("Missing value for flag " + flag);
                }
                return argv[++i];
            };
            if (arg == "--input") {
                input_path = require_val(arg);
            } else if (arg == "--output-features") {
                output_features_path = require_val(arg);
            } else if (arg == "--output-diagnostics") {
                output_diag_path = require_val(arg);
            } else if (arg == "--config") {
                (void)require_val(arg);
            } else if (arg == "--variant") {
                cfg.variant = require_val(arg);
            } else if (arg == "--tick-size") {
                cfg.tick_size = std::stod(require_val(arg));
            } else if (arg == "--stale-threshold-ns") {
                cfg.stale_threshold_ns = std::stoll(require_val(arg));
            } else if (arg == "--flow-window-ns") {
                cfg.flow_window_ns = std::stoll(require_val(arg));
            } else if (arg == "--depth-k") {
                cfg.depth_levels_k = std::stoull(require_val(arg));
            } else if (arg == "--max-levels") {
                cfg.max_levels = std::stoull(require_val(arg));
            } else if (arg == "--max-orders") {
                cfg.max_orders = std::stoull(require_val(arg));
            } else if (arg == "--run-id") {
                run_id = require_val(arg);
            } else if (arg == "--strict-fail-on-invalid") {
                strict_fail = true;
            } else {
                std::cerr << "[ERROR] Unknown argument: " << arg << "\n";
                print_usage();
                return 2;
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] Argument parsing failed: " << ex.what() << "\n";
        return 2;
    }

    if (input_path.empty() || output_features_path.empty() || output_diag_path.empty()) {
        std::cerr << "[ERROR] --input, --output-features, and --output-diagnostics are required.\n";
        print_usage();
        return 2;
    }

    std::string cfg_err;
    if (!mmae::validate_config(cfg, cfg_err)) {
        std::cerr << "[ERROR] Invalid configuration: " << cfg_err << "\n";
        return 2;
    }

    std::vector<mmae::ParsedRow> parsed_rows;
    std::string io_err;
    if (!mmae::load_events_csv(input_path, parsed_rows, io_err)) {
        std::cerr << "[ERROR] " << io_err << "\n";
        return 3;
    }

    mmae::FeatureEngine engine(cfg);
    std::vector<mmae::FeatureRow> features;
    features.reserve(parsed_rows.size());

    // Keep track of CSV parse errors in diagnostics too by routing via an invalid event or direct summary.
    mmae::Diagnostics extra_parse_diag{};
    std::vector<mmae::ValidationErrorRecord> parse_errors;
    uint64_t parse_rejected = 0;

    for (const auto& pr : parsed_rows) {
        if (!pr.ok) {
            ++parse_rejected;
            if (parse_errors.size() < cfg.max_recorded_errors) {
                parse_errors.push_back(mmae::ValidationErrorRecord{
                    pr.line_number, 0, pr.error, pr.error_detail
                });
            }
            continue;
        }
        auto res = engine.process_event(pr.event, pr.line_number);
        if (res.accepted) {
            features.push_back(std::move(res.feature));
        }
    }

    mmae::Diagnostics final_diag = engine.diagnostics();
    final_diag.total_input_rows += parse_rejected;
    final_diag.rejected_events += parse_rejected;
    for (const auto& pe : parse_errors) {
        if (final_diag.errors.size() < cfg.max_recorded_errors) {
            final_diag.errors.push_back(pe);
        }
    }

    if (!mmae::write_features_csv_file(output_features_path, features, io_err)) {
        std::cerr << "[ERROR] " << io_err << "\n";
        return 4;
    }
    if (!mmae::write_diagnostics_json_file(output_diag_path, final_diag, run_id, io_err)) {
        std::cerr << "[ERROR] " << io_err << "\n";
        return 4;
    }

    if (strict_fail && final_diag.rejected_events > 0) {
        std::cerr << "[ERROR] Strict validation failed with " << final_diag.rejected_events
                  << " rejected rows (see " << output_diag_path << ")\n";
        return 5;
    }
    return 0;
}
