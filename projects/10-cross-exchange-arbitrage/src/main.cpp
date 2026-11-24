#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "xarb/audit.hpp"
#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/replay_engine.hpp"
#include "xarb/synth_feed.hpp"

namespace {

void print_usage() {
  std::cout << "xarb — Cross-Exchange Arbitrage Detection & Paper Execution Engine (paper-only)\n\n"
            << "Commands:\n"
            << "  generate --out <path> [--binary] [--steps N] [--seed S] [--no-faults]\n"
            << "  replay   --feed <path> [--out-dir <dir>] [--scenario <name>]\n"
            << "           [--order-latency-ms X] [--order-jitter-ms X] [--feed-delay-ms X]\n"
            << "           [--max-stale-ms X] [--min-edge-bps X] [--min-profit X]\n"
            << "           [--max-notional X] [--initial-quote X] [--initial-base X]\n"
            << "           [--limit-ioc 0|1] [--rebalance 0|1] [--sqlite 0|1] [--seed S]\n"
            << "  sweep    --feed <path> --latencies-ms 0,1,2,5,10,25,50,100 [--out-csv <path>]\n"
            << "           [--limit-ioc 0|1] [--min-edge-bps X]\n"
            << "  convert  --in <path> --out <path> --to binary|jsonl\n"
            << "  inspect  --feed <path>\n";
}

bool get_arg(int argc, char** argv, const std::string& flag, std::string& out) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (flag == argv[i]) {
      out = argv[i + 1];
      return true;
    }
  }
  return false;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 2; i < argc; ++i) {
    if (flag == argv[i]) return true;
  }
  return false;
}

int cmd_generate(int argc, char** argv) {
  std::string out_path;
  if (!get_arg(argc, argv, "--out", out_path)) {
    std::cerr << "error: --out <path> required\n";
    return 1;
  }
  xarb::SynthFeedConfig cfg;
  std::string tmp;
  if (get_arg(argc, argv, "--steps", tmp)) cfg.num_steps = static_cast<size_t>(std::stoull(tmp));
  if (get_arg(argc, argv, "--seed", tmp)) cfg.seed = std::stoull(tmp);
  if (has_flag(argc, argv, "--no-faults")) cfg.inject_faults = false;
  const bool binary = has_flag(argc, argv, "--binary");

  auto reg = xarb::InstrumentRegistry::make_default_crypto_registry();
  auto events = xarb::generate_synthetic_feed(reg, cfg);

  std::string err;
  const bool ok = binary ? xarb::write_binary_feed(out_path, events, &err)
                         : xarb::write_jsonl_feed(out_path, events, &err);
  if (!ok) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  std::cout << "generated " << events.size() << " events -> " << out_path << " (" << (binary ? "binary" : "jsonl")
            << ", seed=" << cfg.seed << ", faults=" << (cfg.inject_faults ? "on" : "off") << ")\n";
  return 0;
}

int cmd_replay(int argc, char** argv) {
  std::string feed_path;
  if (!get_arg(argc, argv, "--feed", feed_path)) {
    std::cerr << "error: --feed <path> required\n";
    return 1;
  }

  std::vector<xarb::RecordedFeedEvent> feed;
  std::string err;
  if (!xarb::read_feed_auto(feed_path, feed, &err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  xarb::ReplayConfig cfg;
  std::string tmp;
  if (get_arg(argc, argv, "--scenario", tmp)) cfg.scenario_name = tmp;
  if (get_arg(argc, argv, "--seed", tmp)) cfg.rng_seed = std::stoull(tmp);
  if (get_arg(argc, argv, "--max-stale-ms", tmp)) {
    cfg.max_stale_ns = static_cast<xarb::TimestampNs>(std::stod(tmp) * 1e6);
  }
  if (get_arg(argc, argv, "--order-latency-ms", tmp)) {
    const auto ns = static_cast<xarb::TimestampNs>(std::stod(tmp) * 1e6);
    cfg.paper.order_latency_ns = {ns, ns, ns};
  }
  if (get_arg(argc, argv, "--order-jitter-ms", tmp)) {
    cfg.paper.order_jitter_ns = static_cast<xarb::TimestampNs>(std::stod(tmp) * 1e6);
  }
  if (get_arg(argc, argv, "--feed-delay-ms", tmp)) {
    const auto ns = static_cast<xarb::TimestampNs>(std::stod(tmp) * 1e6);
    cfg.feed_delay_ns = {ns, ns, ns};
  }
  if (get_arg(argc, argv, "--min-edge-bps", tmp)) cfg.scanner.min_net_edge_bps = std::stod(tmp);
  if (get_arg(argc, argv, "--min-profit", tmp)) cfg.scanner.min_net_profit_quote = std::stod(tmp);
  if (get_arg(argc, argv, "--max-notional", tmp)) cfg.scanner.max_trade_notional_quote = std::stod(tmp);
  if (get_arg(argc, argv, "--initial-quote", tmp)) {
    const double q = std::stod(tmp);
    cfg.paper.initial_quote_per_venue = {q, q, q};
  }
  if (get_arg(argc, argv, "--initial-base", tmp)) {
    const double b = std::stod(tmp);
    cfg.paper.initial_base_per_venue = {b, b, b};
  }
  if (get_arg(argc, argv, "--limit-ioc", tmp)) cfg.paper.use_limit_ioc = (tmp != "0");
  if (get_arg(argc, argv, "--rebalance", tmp)) cfg.paper.enable_rebalance_model = (tmp != "0");

  bool write_sqlite = true;
  if (get_arg(argc, argv, "--sqlite", tmp)) write_sqlite = (tmp != "0");

  auto reg = xarb::InstrumentRegistry::make_default_crypto_registry();
  xarb::ReplayEngine engine(reg, cfg);
  auto res = engine.run(feed, feed_path);

  std::cout << xarb::format_summary_text(res.summary);

  std::string out_dir;
  if (get_arg(argc, argv, "--out-dir", out_dir)) {
    if (!xarb::write_audit_outputs(out_dir, reg, res.summary, res.signals, res.trades, write_sqlite, &err)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    std::cout << "audit outputs      written to " << out_dir << "\n";
  }
  return res.summary.accounting_invariants_ok ? 0 : 2;
}

int cmd_sweep(int argc, char** argv) {
  std::string feed_path;
  std::string lat_str = "0,1,2,5,10,25,50,100";
  std::string out_csv;
  if (!get_arg(argc, argv, "--feed", feed_path)) {
    std::cerr << "error: --feed <path> required\n";
    return 1;
  }
  get_arg(argc, argv, "--latencies-ms", lat_str);
  get_arg(argc, argv, "--out-csv", out_csv);

  std::vector<xarb::RecordedFeedEvent> feed;
  std::string err;
  if (!xarb::read_feed_auto(feed_path, feed, &err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::vector<double> latencies_ms;
  std::stringstream ss(lat_str);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) latencies_ms.push_back(std::stod(tok));
  }

  bool use_limit_ioc = true;
  double min_edge_bps = 2.0;
  std::string tmp;
  if (get_arg(argc, argv, "--limit-ioc", tmp)) use_limit_ioc = (tmp != "0");
  if (get_arg(argc, argv, "--min-edge-bps", tmp)) min_edge_bps = std::stod(tmp);

  auto reg = xarb::InstrumentRegistry::make_default_crypto_registry();

  std::ostringstream csv;
  csv << "order_latency_ms,actionable_signals,trades_attempted,both_profitable,both_loss_slippage,"
         "one_legged_unhedged,both_missed,fill_rate,false_positive_rate,net_pnl_quote,matched_pnl_quote,"
         "unhedged_mtm_quote,fees_quote,max_drawdown_quote,run_hash\n";

  std::cout << std::left << std::setw(10) << "lat_ms" << std::setw(10) << "signals" << std::setw(10) << "profit"
            << std::setw(10) << "slip_ls" << std::setw(10) << "one_leg" << std::setw(10) << "missed"
            << std::setw(11) << "fill_%" << std::setw(11) << "fp_%" << std::setw(13) << "net_pnl"
            << "hash\n";

  for (double lat_ms : latencies_ms) {
    xarb::ReplayConfig cfg;
    cfg.scenario_name = "lat_" + std::to_string(static_cast<int>(lat_ms)) + "ms";
    const auto ns = static_cast<xarb::TimestampNs>(lat_ms * 1e6);
    cfg.paper.order_latency_ns = {ns, ns, ns};
    cfg.paper.order_jitter_ns = (ns > 0) ? std::min<xarb::TimestampNs>(ns / 5, 2LL * xarb::kNsPerMs) : 0;
    cfg.paper.use_limit_ioc = use_limit_ioc;
    cfg.scanner.min_net_edge_bps = min_edge_bps;
    cfg.measure_processing_latency = false;

    xarb::ReplayEngine engine(reg, cfg);
    auto res = engine.run(feed, feed_path);
    const auto& s = res.summary;

    char hash_buf[32];
    std::snprintf(hash_buf, sizeof(hash_buf), "%016llx", static_cast<unsigned long long>(s.run_hash));

    std::cout << std::fixed << std::setprecision(1) << std::left << std::setw(10) << lat_ms << std::setw(10)
              << s.scanner.actionable_signals << std::setw(10) << s.paper.both_filled_profitable << std::setw(10)
              << s.paper.both_filled_loss_slippage << std::setw(10) << s.paper.one_legged_unhedged << std::setw(10)
              << s.paper.both_missed_expired << std::setprecision(2) << std::setw(11) << (s.fill_rate * 100.0)
              << std::setw(11) << (s.false_positive_rate * 100.0) << std::setw(13) << s.paper.net_simulated_pnl_quote
              << hash_buf << "\n";

    csv << std::fixed << std::setprecision(4) << lat_ms << ',' << s.scanner.actionable_signals << ','
        << s.paper.trades_attempted << ',' << s.paper.both_filled_profitable << ','
        << s.paper.both_filled_loss_slippage << ',' << s.paper.one_legged_unhedged << ','
        << s.paper.both_missed_expired << ',' << s.fill_rate << ',' << s.false_positive_rate << ','
        << s.paper.net_simulated_pnl_quote << ',' << s.paper.total_realized_matched_pnl_quote << ','
        << s.paper.total_unhedged_mtm_pnl_quote << ',' << s.paper.total_fees_quote << ','
        << s.paper.max_drawdown_quote << ',' << hash_buf << "\n";
  }

  if (!out_csv.empty()) {
    std::ofstream ofs(out_csv, std::ios::trunc);
    if (!ofs) {
      std::cerr << "error: failed to open " << out_csv << "\n";
      return 1;
    }
    ofs << csv.str();
    std::cout << "sweep csv          written to " << out_csv << "\n";
  }
  return 0;
}

int cmd_convert(int argc, char** argv) {
  std::string in_path, out_path, to_fmt;
  if (!get_arg(argc, argv, "--in", in_path) || !get_arg(argc, argv, "--out", out_path) ||
      !get_arg(argc, argv, "--to", to_fmt)) {
    std::cerr << "error: --in, --out, and --to binary|jsonl required\n";
    return 1;
  }
  std::vector<xarb::RecordedFeedEvent> feed;
  std::string err;
  if (!xarb::read_feed_auto(in_path, feed, &err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  bool ok = false;
  if (to_fmt == "binary") ok = xarb::write_binary_feed(out_path, feed, &err);
  else if (to_fmt == "jsonl") ok = xarb::write_jsonl_feed(out_path, feed, &err);
  else {
    std::cerr << "error: --to must be 'binary' or 'jsonl'\n";
    return 1;
  }
  if (!ok) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  std::cout << "converted " << feed.size() << " events -> " << out_path << " (" << to_fmt << ")\n";
  return 0;
}

int cmd_inspect(int argc, char** argv) {
  std::string feed_path;
  if (!get_arg(argc, argv, "--feed", feed_path)) {
    std::cerr << "error: --feed <path> required\n";
    return 1;
  }
  std::vector<xarb::RecordedFeedEvent> feed;
  std::string err;
  if (!xarb::read_feed_auto(feed_path, feed, &err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  std::array<uint64_t, xarb::kVenueCount> by_venue{};
  for (const auto& ev : feed) {
    by_venue[static_cast<size_t>(ev.venue)] += 1;
  }
  double span_s = 0.0;
  if (feed.size() >= 2) {
    span_s = static_cast<double>(feed.back().recv_ns - feed.front().recv_ns) / 1e9;
  }
  std::cout << "feed: " << feed_path << "\n"
            << "  events: " << feed.size() << "\n"
            << "  span:   " << std::fixed << std::setprecision(3) << span_s << " s\n"
            << "  binance=" << by_venue[0] << " coinbase=" << by_venue[1] << " okx=" << by_venue[2] << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const std::string cmd = argv[1];
  if (cmd == "generate") return cmd_generate(argc, argv);
  if (cmd == "replay") return cmd_replay(argc, argv);
  if (cmd == "sweep") return cmd_sweep(argc, argv);
  if (cmd == "convert") return cmd_convert(argc, argv);
  if (cmd == "inspect") return cmd_inspect(argc, argv);
  if (cmd == "-h" || cmd == "--help") {
    print_usage();
    return 0;
  }
  std::cerr << "unknown command: " << cmd << "\n";
  print_usage();
  return 1;
}
