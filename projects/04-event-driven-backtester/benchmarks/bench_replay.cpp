// Replay benchmark. Prints one CSV row per scenario to stdout (or --out).
//
//   bench_replay [--events N] [--reps R] [--seed S] [--out FILE] [--only PREFIX]
//   bench_replay --scale N        # single big run: wall time + peak RSS
//
// Input is seeded synthetic L3 flow generated in-process, so a run is fully
// described by (source revision, flags, machine). Feed generation and file
// I/O are outside every timed region.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hftbt/engine.hpp"
#include "hftbt/strategies.hpp"
#include "hftbt/synth.hpp"

using namespace hftbt;
using Clock = std::chrono::steady_clock;

namespace {

struct Scenario {
    std::string name;
    std::string strategy; // noop | market_maker | imbalance | ladder
    bool fifo = true;
    std::uint32_t instruments = 1;
    std::uint32_t ladder_levels = 0;
    Timestamp md_delay_ns = 50'000;
};

std::unique_ptr<Strategy> make(const Scenario& sc) {
    if (sc.strategy == "market_maker") return std::make_unique<MarketMaker>(MarketMakerParams{});
    if (sc.strategy == "imbalance") return std::make_unique<ImbalanceTaker>(ImbalanceParams{});
    if (sc.strategy == "ladder") {
        // Orders far larger than any traded volume: they collect partial fills
        // but never complete, so the number resting stays at 2 * levels.
        LadderParams p;
        p.levels = sc.ladder_levels;
        p.order_qty = 1'000'000'000;
        p.offset_ticks = 1;
        return std::make_unique<Ladder>(p);
    }
    return std::make_unique<NoopStrategy>();
}

EngineConfig config(const Scenario& sc) {
    EngineConfig c;
    c.latency.md_delay_ns = sc.md_delay_ns;
    c.latency.compute_delay_ns = 2'000;
    c.latency.order_delay_ns = 100'000;
    c.latency.response_delay_ns = 100'000;
    c.fees.maker_bps = -0.1;
    c.fees.taker_bps = 0.3;
    c.risk.max_order_qty = 1'000'000'000'000;
    c.risk.max_position = 1'000'000'000'000'000;
    c.risk.max_open_orders = 1'000'000;
    c.md_fifo = sc.fifo;
    c.record_fills = false;
    c.sample_interval_ns = 100'000'000;
    return c;
}

double peak_rss_mb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0); // bytes
#else
    return static_cast<double>(ru.ru_maxrss) / 1024.0; // kilobytes
#endif
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// Smallest non-zero difference the clock can report: per-event latencies
// below are quantised to this.
long long clock_tick_ns() {
    long long best = 1'000'000;
    for (int i = 0; i < 100000; ++i) {
        const auto a = Clock::now();
        auto b = Clock::now();
        while (b == a) b = Clock::now();
        best = std::min<long long>(
            best, std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }
    return best;
}

int scale_run(std::size_t events, std::uint64_t seed) {
    const FeedData feed = generate_l3({.seed = seed, .events = events});
    const double rss_feed = peak_rss_mb();
    Scenario sc{"scale", "market_maker"};
    std::uint64_t hash[2];
    double best = 1e300;
    std::uint64_t dispatched = 0;
    for (std::uint64_t& h : hash) { // twice: the second run must reproduce the first
        const auto strat = make(sc);
        Engine engine(feed, config(sc), *strat);
        const auto t0 = Clock::now();
        engine.run();
        best = std::min(best, seconds(Clock::now() - t0));
        h = engine.summary().event_hash;
        dispatched = engine.summary().dispatched_events;
    }
    std::printf("%zu,%llu,%.4f,%.0f,%.1f,%.1f,%s,%016llx\n", events,
                static_cast<unsigned long long>(dispatched), best,
                static_cast<double>(dispatched) / best, rss_feed, peak_rss_mb(),
                hash[0] == hash[1] ? "yes" : "NO", static_cast<unsigned long long>(hash[0]));
    return hash[0] == hash[1] ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t events = 2'000'000;
    int reps = 5;
    std::uint64_t seed = 42;
    std::string out_path, only;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--events") && more)
            events = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--reps") && more)
            reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seed") && more)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && more)
            out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--only") && more)
            only = argv[++i];
        else if (!std::strcmp(argv[i], "--scale") && more)
            return scale_run(std::strtoull(argv[++i], nullptr, 10), seed);
        else {
            std::fprintf(stderr, "usage: bench_replay [--events N] [--reps R] [--seed S] "
                                 "[--out FILE] [--only PREFIX] | --scale N\n");
            return 2;
        }
    }

    std::vector<Scenario> scenarios = {
        {"strategy/noop", "noop"},
        {"strategy/market_maker", "market_maker"},
        {"strategy/imbalance", "imbalance"},
    };
    // Tradeoff under test: where delayed market data waits. The number of
    // events in flight grows with md_delay / inter-event gap.
    for (const Timestamp d :
         {Timestamp{0}, Timestamp{50'000}, Timestamp{5'000'000}, Timestamp{500'000'000}})
        for (const bool fifo : {true, false})
            scenarios.push_back({std::string("md_queue/") + (fifo ? "fifo" : "heap") + "/delay_" +
                                     std::to_string(d),
                                 "market_maker", fifo, 1, 0, d});
    for (const std::uint32_t n : {1u, 4u, 16u, 64u, 256u})
        scenarios.push_back({"instruments/" + std::to_string(n), "market_maker", true, n});
    for (const std::uint32_t l : {0u, 1u, 8u, 64u, 512u, 4096u})
        scenarios.push_back({"active_orders/" + std::to_string(2 * l), "ladder", true, 1, l});

    std::FILE* out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::fprintf(stderr, "clock tick: %lld ns\n", clock_tick_ns());
    std::fprintf(out, "scenario,strategy,md_queue,instruments,md_delay_ns,feed_events,"
                      "dispatched_events,strategy_callbacks,open_orders_at_end,fills,reps,"
                      "best_s,median_s,dispatched_per_s,feed_events_per_s,"
                      "p50_ns,p99_ns,p999_ns,max_ns,event_hash\n");

    std::map<std::uint32_t, FeedData> feeds;
    for (const Scenario& sc : scenarios) {
        if (!only.empty() && sc.name.rfind(only, 0) != 0) continue;
        if (!feeds.contains(sc.instruments))
            feeds[sc.instruments] =
                generate_l3({.seed = seed, .events = events, .instruments = sc.instruments});
        const FeedData& feed = feeds[sc.instruments];

        // Pass 1: throughput, no instrumentation inside the loop.
        std::vector<double> times;
        Summary sum;
        for (int r = 0; r < reps; ++r) {
            const auto strat = make(sc);
            Engine engine(feed, config(sc), *strat);
            const auto t0 = Clock::now();
            engine.run();
            times.push_back(seconds(Clock::now() - t0));
            if (r > 0 && engine.summary().event_hash != sum.event_hash) {
                std::fprintf(stderr, "non-deterministic run in %s\n", sc.name.c_str());
                return 1;
            }
            sum = engine.summary();
        }
        std::sort(times.begin(), times.end());

        // Pass 2: per-event dispatch latency (clock calls inflate each sample).
        std::vector<std::uint32_t> lat;
        lat.reserve(sum.dispatched_events);
        {
            const auto strat = make(sc);
            Engine engine(feed, config(sc), *strat);
            engine.start();
            for (;;) {
                const auto t0 = Clock::now();
                const bool more = engine.step();
                const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
                if (!more) break;
                lat.push_back(static_cast<std::uint32_t>(std::min<long long>(ns, UINT32_MAX)));
            }
            engine.finish();
        }
        std::sort(lat.begin(), lat.end());
        const auto pct = [&](double p) {
            return lat.empty()
                       ? 0u
                       : lat[static_cast<std::size_t>(p * static_cast<double>(lat.size() - 1))];
        };

        std::fprintf(
            out,
            "%s,%s,%s,%u,%lld,%llu,%llu,%llu,%zu,%llu,%d,%.4f,%.4f,%.0f,%.0f,%u,%u,%u,%u,%016llx\n",
            sc.name.c_str(), sc.strategy.c_str(), sc.fifo ? "fifo" : "heap", sc.instruments,
            static_cast<long long>(sc.md_delay_ns),
            static_cast<unsigned long long>(sum.feed_events),
            static_cast<unsigned long long>(sum.dispatched_events),
            static_cast<unsigned long long>(sum.strategy_callbacks), sum.open_orders_at_end,
            static_cast<unsigned long long>(sum.fills), reps, times.front(),
            times[times.size() / 2], static_cast<double>(sum.dispatched_events) / times.front(),
            static_cast<double>(sum.feed_events) / times.front(), pct(0.50), pct(0.99), pct(0.999),
            lat.empty() ? 0u : lat.back(), static_cast<unsigned long long>(sum.event_hash));
        std::fflush(out);
        std::fprintf(stderr, "%-34s %7.2f M dispatched/s  p50 %u ns  p99 %u ns  p99.9 %u ns\n",
                     sc.name.c_str(),
                     static_cast<double>(sum.dispatched_events) / times.front() * 1e-6, pct(0.50),
                     pct(0.99), pct(0.999));
    }
    if (out != stdout) std::fclose(out);
    return 0;
}
