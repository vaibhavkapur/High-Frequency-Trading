// hftbt: command-line front end.
//   hftbt run --config FILE [--set section.key=value]... [--quiet | --json]
//   hftbt gen --out FILE [--events N] [--instruments K] [--seed S] [--gap-ns G] [--l2]
//   hftbt convert IN OUT [--to-l2]
//   hftbt info FILE
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hftbt/config.hpp"
#include "hftbt/synth.hpp"

using namespace hftbt;

namespace {

int usage() {
    std::fputs(
        "usage:\n"
        "  hftbt run --config FILE [--set section.key=value]... [--quiet | --json]\n"
        "  hftbt gen --out FILE [--events N] [--instruments K] [--seed S] [--gap-ns G] [--l2]\n"
        "  hftbt convert IN OUT [--to-l2]\n"
        "  hftbt info FILE\n"
        "Feed files ending in .bin are binary; anything else is CSV.\n",
        stderr);
    return 2;
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
    return 1;
}

int cmd_run(const std::vector<std::string>& args) {
    Config cfg;
    std::string err;
    bool quiet = false, json = false, have_config = false;
    std::vector<std::string> sets;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i + 1 < args.size()) {
            if (!cfg.load(args[++i], err)) return fail(err);
            have_config = true;
        } else if (args[i] == "--set" && i + 1 < args.size()) {
            sets.push_back(args[++i]);
        } else if (args[i] == "--quiet") {
            quiet = true; // print only the event hash
        } else if (args[i] == "--json") {
            json = true; // print summary.json to stdout
        } else {
            return usage();
        }
    }
    if (!have_config && sets.empty()) return usage();
    for (const std::string& s : sets) // overrides win regardless of argument order
        if (!cfg.set(s, err)) return fail(err);

    const std::string data = cfg.get_str("run.data");
    const std::string out_dir = cfg.get_str("run.out_dir");
    const EngineConfig ecfg = make_engine_config(cfg);
    const std::unique_ptr<Strategy> strategy = make_strategy(cfg, err);
    if (!strategy) return fail(err);
    if (const auto problems = cfg.problems(); !problems.empty()) {
        for (const std::string& p : problems) std::fprintf(stderr, "error: %s\n", p.c_str());
        return 1;
    }
    if (data.empty()) return fail("run.data is not set");

    FeedData feed;
    if (!load_feed(data, feed, err)) return fail(err);

    Engine engine(feed, ecfg, *strategy);
    const auto t0 = std::chrono::steady_clock::now();
    engine.run();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (!out_dir.empty() && !write_reports(engine, out_dir, err)) return fail(err);
    const Summary& s = engine.summary();
    if (json) {
        std::fputs(summary_json(s, feed.symbols).c_str(), stdout);
        return 0;
    }
    if (quiet) {
        std::printf("%016llx\n", static_cast<unsigned long long>(s.event_hash));
        return 0;
    }
    std::printf("feed             %s (%zu events, %zu instruments)\n", data.c_str(),
                feed.events.size(), feed.symbols.size());
    std::printf("strategy         %s\n", cfg.get_str("strategy.name", "noop").c_str());
    std::printf("simulated span   %.3f s\n", static_cast<double>(s.end_ts - s.start_ts) * 1e-9);
    std::printf("dispatched       %llu events in %.3f s wall (%.2f M events/s)\n",
                static_cast<unsigned long long>(s.dispatched_events), secs,
                secs > 0 ? static_cast<double>(s.dispatched_events) / secs * 1e-6 : 0.0);
    std::printf("orders           sent %llu, risk-rejected %llu, exch-rejected %llu, cancels %llu "
                "(%llu too late)\n",
                static_cast<unsigned long long>(s.orders_sent),
                static_cast<unsigned long long>(s.orders_risk_rejected),
                static_cast<unsigned long long>(s.orders_exchange_rejected),
                static_cast<unsigned long long>(s.cancels_sent),
                static_cast<unsigned long long>(s.cancel_rejects));
    std::printf("fills            %llu (%llu maker / %llu taker), fill ratio %.4f\n",
                static_cast<unsigned long long>(s.fills),
                static_cast<unsigned long long>(s.maker_fills),
                static_cast<unsigned long long>(s.taker_fills), s.fill_ratio);
    std::printf("pnl net / gross  %.2f / %.2f (fees %.2f)\n", s.pnl_net, s.pnl_gross, s.fees);
    std::printf("turnover         %.2f\n", s.turnover);
    std::printf("max drawdown     %.2f\n", s.max_drawdown);
    std::printf("gross exposure   avg %.2f, max %.2f\n", s.avg_gross_exposure,
                s.max_gross_exposure);
    for (const MarkoutStats& m : s.markouts)
        std::printf("markout %-9lld maker %+.4f ticks/lot (%lld lots), taker %+.4f (%lld lots)\n",
                    static_cast<long long>(m.horizon_ns),
                    m.maker_qty ? m.maker_sum / static_cast<double>(m.maker_qty) : 0.0,
                    static_cast<long long>(m.maker_qty),
                    m.taker_qty ? m.taker_sum / static_cast<double>(m.taker_qty) : 0.0,
                    static_cast<long long>(m.taker_qty));
    if (s.data_anomalies)
        std::printf("data anomalies   %llu feed events ignored as inconsistent\n",
                    static_cast<unsigned long long>(s.data_anomalies));
    std::printf("event hash       %016llx\n", static_cast<unsigned long long>(s.event_hash));
    if (!out_dir.empty()) std::printf("reports          %s/\n", out_dir.c_str());
    return 0;
}

int cmd_gen(const std::vector<std::string>& args) {
    SynthConfig sc;
    std::string out, err;
    bool l2 = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const bool more = i + 1 < args.size();
        if (args[i] == "--out" && more)
            out = args[++i];
        else if (args[i] == "--events" && more)
            sc.events = std::strtoull(args[++i].c_str(), nullptr, 10);
        else if (args[i] == "--instruments" && more)
            sc.instruments =
                static_cast<std::uint32_t>(std::strtoul(args[++i].c_str(), nullptr, 10));
        else if (args[i] == "--seed" && more)
            sc.seed = std::strtoull(args[++i].c_str(), nullptr, 10);
        else if (args[i] == "--gap-ns" && more)
            sc.mean_gap_ns = std::strtoll(args[++i].c_str(), nullptr, 10);
        else if (args[i] == "--l2")
            l2 = true;
        else
            return usage();
    }
    if (out.empty()) return usage();
    FeedData feed = generate_l3(sc);
    if (l2) feed = l3_to_l2(feed);
    if (!save_feed(out, feed, err)) return fail(err);
    std::printf("wrote %zu synthetic %s events for %zu instrument(s) to %s (seed %llu)\n",
                feed.events.size(), l2 ? "L2" : "L3", feed.symbols.size(), out.c_str(),
                static_cast<unsigned long long>(sc.seed));
    return 0;
}

int cmd_convert(const std::vector<std::string>& args) {
    std::vector<std::string> paths;
    bool to_l2 = false;
    for (const std::string& a : args) {
        if (a == "--to-l2")
            to_l2 = true;
        else
            paths.push_back(a);
    }
    if (paths.size() != 2) return usage();
    FeedData feed;
    std::string err;
    if (!load_feed(paths[0], feed, err)) return fail(err);
    if (to_l2) feed = l3_to_l2(feed);
    if (!save_feed(paths[1], feed, err)) return fail(err);
    std::printf("wrote %zu events to %s\n", feed.events.size(), paths[1].c_str());
    return 0;
}

// One JSON object describing a feed file; used by the sweep tooling.
int cmd_info(const std::vector<std::string>& args) {
    if (args.size() != 1) return usage();
    FeedData feed;
    std::string err;
    if (!load_feed(args[0], feed, err)) return fail(err);
    std::printf("{\"events\": %zu, \"instruments\": %zu, \"start_ts\": %lld, \"end_ts\": %lld}\n",
                feed.events.size(), feed.symbols.size(),
                static_cast<long long>(feed.events.empty() ? 0 : feed.events.front().ts),
                static_cast<long long>(feed.events.empty() ? 0 : feed.events.back().ts));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);
    if (cmd == "run") return cmd_run(args);
    if (cmd == "gen") return cmd_gen(args);
    if (cmd == "convert") return cmd_convert(args);
    if (cmd == "info") return cmd_info(args);
    return usage();
}
