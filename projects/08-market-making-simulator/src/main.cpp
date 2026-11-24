#include "mmsim/config.hpp"
#include "mmsim/engine.hpp"
#include "mmsim/feed.hpp"
#include "mmsim/synth.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout << "Usage:\n"
              << "  mmsim generate --out <path.{mmbin,csv}> [--regime mean_reverting|toxic_trend|high_vol_burst|thin_book]\n"
              << "                 [--seed <u64>] [--duration <sec>] [--rate <hz>] [--vol <ticks/sqrt(s)>]\n"
              << "                 [--informed-prob <0..1>] [--tick-size <dollars>]\n"
              << "  mmsim simulate --input <path.{mmbin,csv}> [--config <cfg>] [--set key=val ...]\n"
              << "                 [--out-dir <dir>] [--quiet]\n";
}

void print_summary(const mmsim::SimSummary& s) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "=== Market-Making Simulation Summary ===\n";
    std::cout << "Strategy / Fill / Book : " << s.strategy_name << " / " << s.fill_model_name
              << " / " << s.book_impl_name << "\n";
    std::cout << "Market Events / Dur(s) : " << s.total_market_events << " / "
              << std::setprecision(2) << s.session_duration_sec << " s\n";
    std::cout << "Quotes (sub/acc/rrej/erej): " << s.quotes_submitted << " / " << s.quotes_accepted
              << " / " << s.quotes_risk_rejected << " / " << s.quotes_exch_rejected << "\n";
    std::cout << "Cancels (sub/ok/late/race): " << s.cancels_submitted << " / "
              << s.cancels_succeeded << " / " << s.cancels_too_late << " / "
              << s.cancel_races_filled << "\n";
    std::cout << "Fills (tot/maker/taker): " << s.fill_count << " / " << s.maker_fills << " / "
              << s.taker_fills << " (" << s.total_filled_shares << " sh, fill rate "
              << std::setprecision(2) << (100.0 * s.order_fill_rate) << "%)\n";
    std::cout << "Inventory (end/max/avg/rms): " << s.final_position << " / " << s.max_abs_position
              << " / " << std::setprecision(1) << s.mean_abs_position << " / " << s.rms_position
              << " sh\n";
    std::cout << std::setprecision(2);
    std::cout << "Gross / Fees / Net MTM : $" << s.gross_mtm_pnl_dollars << " / $"
              << s.total_fees_dollars << " / $" << s.net_mtm_pnl_dollars << "\n";
    std::cout << "Realized / Unrealized  : $" << s.realized_pnl_dollars << " / $"
              << s.unrealized_pnl_dollars << " | Max DD: $" << s.max_drawdown_dollars << "\n";
    std::cout << "Captured Half-Spread   : " << std::setprecision(3)
              << s.mean_captured_half_spread_ticks << " ticks (" << s.mean_captured_half_spread_bps
              << " bps)\n";
    std::cout << "Reconciliation Error   : $" << std::scientific << std::setprecision(2)
              << s.reconciliation_error_dollars << std::fixed << "\n";
    if (!s.markouts.empty()) {
        std::cout << "Adverse-Selection Markouts (only fills with full horizon timestamp support):\n";
        for (const auto& m : s.markouts) {
            std::cout << "  h=" << std::setw(6) << std::setprecision(0)
                      << (static_cast<double>(m.horizon_ns) / 1e6) << " ms"
                      << " | N=" << std::setw(5) << m.valid_fills << " fills"
                      << " | adv_sel=" << std::setprecision(3) << m.mean_adverse_selection_ticks
                      << " ticks (" << std::setprecision(2) << m.mean_adverse_selection_bps << " bps)"
                      << " | realized_spr=" << std::setprecision(3) << m.mean_realized_spread_ticks
                      << " ticks (" << std::setprecision(2) << m.mean_realized_spread_bps << " bps)\n";
        }
    }
}

int cmd_generate(int argc, char** argv) {
    mmsim::SynthParams p{};
    std::string out_path;
    double tick_size = 0.01;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--out") {
            out_path = need(arg);
        } else if (arg == "--regime") {
            const std::string r = need(arg);
            if (!mmsim::parse_regime(r, p.regime)) {
                std::cerr << "Unknown regime: " << r << "\n";
                return 2;
            }
        } else if (arg == "--seed") {
            p.seed = std::stoull(need(arg));
        } else if (arg == "--duration") {
            p.duration_sec = std::stod(need(arg));
        } else if (arg == "--rate") {
            p.base_event_rate_hz = std::stod(need(arg));
        } else if (arg == "--vol") {
            p.vol_ticks_per_sqrt_sec = std::stod(need(arg));
        } else if (arg == "--informed-prob") {
            p.informed_flow_prob = std::stod(need(arg));
        } else if (arg == "--tick-size") {
            tick_size = std::stod(need(arg));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (out_path.empty()) {
        std::cerr << "Error: --out is required\n";
        return 2;
    }

    const auto events = mmsim::generate_synthetic_session(p);
    std::string err;
    bool ok = false;
    if (out_path.size() >= 6 && out_path.substr(out_path.size() - 6) == ".mmbin") {
        ok = mmsim::save_market_events_bin(out_path, events, tick_size, err);
    } else {
        ok = mmsim::save_market_events_csv(out_path, events, err);
    }
    if (!ok) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    std::cout << "Generated " << events.size() << " events (" << mmsim::regime_to_string(p.regime)
              << ", seed=" << p.seed << ") -> " << out_path << "\n";
    return 0;
}

int cmd_simulate(int argc, char** argv) {
    std::string input_path;
    std::string config_path;
    std::string out_dir;
    bool quiet = false;
    std::vector<std::string> overrides;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--input") {
            input_path = need(arg);
        } else if (arg == "--config") {
            config_path = need(arg);
        } else if (arg == "--out-dir") {
            out_dir = need(arg);
        } else if (arg == "--set") {
            overrides.push_back(need(arg));
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: --input is required\n";
        return 2;
    }

    mmsim::SimConfig cfg{};
    std::string err;
    if (!config_path.empty()) {
        if (!mmsim::load_config_file(config_path, cfg, err)) {
            std::cerr << "Config error: " << err << "\n";
            return 1;
        }
    }

    std::vector<mmsim::MarketEvent> events;
    double file_tick = 0.0;
    if (!mmsim::load_market_events(input_path, events, &file_tick, err)) {
        std::cerr << "Feed error: " << err << "\n";
        return 1;
    }
    if (file_tick > 0.0) {
        cfg.tick_size_dollars = file_tick;
        cfg.strategy.tick_size_dollars = file_tick;
    }

    for (const auto& kv : overrides) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Invalid --set argument (expected key=value): " << kv << "\n";
            return 2;
        }
        if (!mmsim::apply_config_kv(kv.substr(0, eq), kv.substr(eq + 1), cfg, err)) {
            std::cerr << "Override error: " << err << "\n";
            return 2;
        }
    }

    mmsim::SimulatorEngine engine(cfg);
    const mmsim::SimResult res = engine.run(events, true);

    if (!quiet) {
        print_summary(res.summary);
    }

    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        const std::string sum_path = out_dir + "/summary.json";
        const std::string fills_path = out_dir + "/fills.csv";
        const std::string eq_path = out_dir + "/equity.csv";
        if (!mmsim::write_summary_json(sum_path, res.summary, cfg, err) ||
            !mmsim::write_fills_csv(fills_path, res.fills, cfg.tick_size_dollars, err) ||
            !mmsim::write_equity_csv(eq_path, res.equity_curve, err)) {
            std::cerr << "Output write error: " << err << "\n";
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "generate") {
        return cmd_generate(argc, argv);
    }
    if (cmd == "simulate") {
        return cmd_simulate(argc, argv);
    }
    if (cmd == "-h" || cmd == "--help") {
        print_usage();
        return 0;
    }
    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage();
    return 2;
}
