#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/quote_normalizer.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

namespace {

struct BenchResult {
    std::string name{};
    std::size_t venues{3};
    std::size_t depth{5};
    std::size_t warmup{0};
    std::size_t samples{0};
    double throughput_ops_per_sec{0.0};
    double p50_ns{0.0};
    double p90_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double max_ns{0.0};
    double mean_ns{0.0};
    std::int64_t sink{0};
};

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

std::vector<sor::VenueConfig> make_n_venues(std::size_t n) {
    auto base = sor::make_default_config().venues;
    std::vector<sor::VenueConfig> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        sor::VenueConfig v = base[i % base.size()];
        v.id = static_cast<sor::VenueId>(i);
        v.name = "VENUE_" + std::to_string(i);
        v.priority = static_cast<std::uint32_t>(i);
        v.base_one_way_latency_ns = 100'000 + static_cast<sor::TimestampNs>(i) * 80'000;
        v.taker_fee_per_share_ticks = static_cast<sor::PriceTicks>((i % 2 == 0) ? 25 : -15);
        out.push_back(v);
    }
    return out;
}

BenchResult summarize(const std::string& name, std::size_t venues, std::size_t depth,
                      std::size_t warmup, std::vector<double>& lat_ns, double wall_sec,
                      std::int64_t sink) {
    BenchResult r{};
    r.name = name;
    r.venues = venues;
    r.depth = depth;
    r.warmup = warmup;
    r.samples = lat_ns.size();
    r.sink = sink;
    r.throughput_ops_per_sec = wall_sec > 0.0 ? (static_cast<double>(lat_ns.size()) / wall_sec) : 0.0;

    double sum = 0.0;
    for (double x : lat_ns) sum += x;
    r.mean_ns = lat_ns.empty() ? 0.0 : (sum / static_cast<double>(lat_ns.size()));

    std::sort(lat_ns.begin(), lat_ns.end());
    r.p50_ns = percentile(lat_ns, 0.50);
    r.p90_ns = percentile(lat_ns, 0.90);
    r.p99_ns = percentile(lat_ns, 0.99);
    r.p999_ns = percentile(lat_ns, 0.999);
    r.max_ns = lat_ns.empty() ? 0.0 : lat_ns.back();
    return r;
}

BenchResult bench_route_plan(std::size_t num_venues, std::size_t depth, std::size_t warmup,
                             std::size_t samples, std::uint64_t seed) {
    using namespace sor;
    auto venues = make_n_venues(num_venues);
    ConsolidatedBook book;
    book.configure_venues(venues);
    VenueHealthMonitor health;
    RouterConfig rcfg{};
    rcfg.policy = RoutingPolicy::SmartCostAware;
    RoutePlanner planner(rcfg);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Quantity> qty_dist(100, 1500);
    std::uniform_int_distribution<int> tick_jit(-2, 2);

    const TimestampNs now_ns = 5'000'000'000;
    for (const auto& v : venues) {
        std::array<PriceLevel, kMaxBookDepth> bids{};
        std::array<PriceLevel, kMaxBookDepth> asks{};
        for (std::size_t d = 0; d < depth; ++d) {
            bids[d] = PriceLevel{1'500'000 - 50 - static_cast<PriceTicks>(d) * 100 +
                                     tick_jit(rng) * 10,
                                 200 + static_cast<Quantity>(d) * 100, 2};
            asks[d] = PriceLevel{1'500'000 + 50 + static_cast<PriceTicks>(d) * 100 +
                                     tick_jit(rng) * 10,
                                 200 + static_cast<Quantity>(d) * 100, 2};
        }
        NormalizedQuoteUpdate ub{};
        ub.venue_id = v.id;
        ub.exchange_ts_ns = now_ns - 100'000;
        ub.recv_ts_ns = now_ns - 50'000;
        ub.type = QuoteUpdateType::Snapshot;
        ub.side = Side::Buy;
        ub.level_count = static_cast<std::uint8_t>(depth);
        ub.levels = bids;
        book.apply_update(ub);

        NormalizedQuoteUpdate ua = ub;
        ua.side = Side::Sell;
        ua.levels = asks;
        book.apply_update(ua);
    }

    // Pre-generate requests to avoid measuring PRNG overhead.
    std::vector<RouteRequest> reqs(warmup + samples);
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        reqs[i].parent_id = i + 1;
        reqs[i].side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        reqs[i].qty = qty_dist(rng);
        reqs[i].limit_price = (reqs[i].side == Side::Buy) ? 1'510'000 : 1'490'000;
        reqs[i].tif = TimeInForce::IOC;
        reqs[i].now_ns = now_ns;
    }

    volatile std::int64_t sink = 0;
    for (std::size_t i = 0; i < warmup; ++i) {
        const auto plan = planner.plan(book, &health, reqs[i]);
        sink += plan.planned_qty;
    }

    // Batch size of 16 ops per steady_clock sample keeps timer overhead below resolution noise
    // on Apple Silicon (~41.67ns cntvct_el0 tick) while measuring per-decision latency accurately!
    constexpr std::size_t kBatch = 16;
    const std::size_t num_batches = samples / kBatch;
    std::vector<double> lat_ns;
    lat_ns.reserve(num_batches);

    const auto t_start = std::chrono::steady_clock::now();
    for (std::size_t b = 0; b < num_batches; ++b) {
        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < kBatch; ++k) {
            const auto plan = planner.plan(book, &health, reqs[warmup + b * kBatch + k]);
            sink += plan.planned_qty;
        }
        const auto b1 = std::chrono::steady_clock::now();
        const double ns =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count()) /
            static_cast<double>(kBatch);
        lat_ns.push_back(ns);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double wall_sec =
        std::chrono::duration<double>(t_end - t_start).count();

    auto r = summarize("route_plan_only", num_venues, depth, warmup, lat_ns, wall_sec, sink);
    r.samples = num_batches * kBatch;
    r.throughput_ops_per_sec = wall_sec > 0.0 ? (static_cast<double>(r.samples) / wall_sec) : 0.0;
    return r;
}

BenchResult bench_wire_update_and_route(std::size_t num_venues, std::size_t depth,
                                        std::size_t warmup, std::size_t samples,
                                        std::uint64_t seed) {
    using namespace sor;
    auto venues = make_n_venues(num_venues);
    QuoteNormalizer normalizer;
    ConsolidatedBook book;
    book.configure_venues(venues);
    for (const auto& v : venues) normalizer.register_venue(v.id);

    VenueHealthMonitor health;
    RouterConfig rcfg{};
    rcfg.policy = RoutingPolicy::SmartCostAware;
    RoutePlanner planner(rcfg);

    std::mt19937_64 rng(seed ^ 0x55AA55AAULL);
    std::uniform_int_distribution<Quantity> qty_dist(100, 1200);
    std::uniform_int_distribution<int> tick_jit(-2, 2);

    TimestampNs ts = 5'000'000'000;
    std::array<std::uint64_t, kMaxVenues> seqs{};
    for (const auto& v : venues) {
        for (Side s : {Side::Buy, Side::Sell}) {
            NormalizedQuoteUpdate u{};
            u.venue_id = v.id;
            u.seq = ++seqs[v.id];
            u.exchange_ts_ns = ts - 100'000;
            u.recv_ts_ns = ts - 50'000;
            u.type = QuoteUpdateType::Snapshot;
            u.side = s;
            u.level_count = static_cast<std::uint8_t>(depth);
            for (std::size_t d = 0; d < depth; ++d) {
                const PriceTicks px =
                    (s == Side::Buy)
                        ? (1'500'000 - 50 - static_cast<PriceTicks>(d) * 100)
                        : (1'500'000 + 50 + static_cast<PriceTicks>(d) * 100);
                u.levels[d] = PriceLevel{px, 300, 2};
            }
            NormalizedQuoteUpdate norm{};
            (void)normalizer.normalize(u, norm);
            book.apply_update(norm);
        }
    }

    // Pre-encode wire packets so we benchmark decode + normalize + book update + route plan.
    const std::size_t total = warmup + samples;
    std::vector<std::vector<std::uint8_t>> packets;
    packets.reserve(total);
    std::vector<RouteRequest> reqs(total);
    for (std::size_t i = 0; i < total; ++i) {
        ts += 1'000;
        const VenueId vid = static_cast<VenueId>(i % num_venues);
        NormalizedQuoteUpdate u{};
        u.venue_id = vid;
        u.symbol_id = 1;
        u.seq = ++seqs[vid];
        u.exchange_ts_ns = ts - 50'000;
        u.recv_ts_ns = ts;
        u.type = QuoteUpdateType::UpsertLevel;
        u.side = (i % 2 == 0) ? Side::Sell : Side::Buy;
        u.level_count = 1;
        const PriceTicks px = (u.side == Side::Sell) ? (1'500'050 + tick_jit(rng) * 10)
                                                     : (1'499'950 + tick_jit(rng) * 10);
        u.levels[0] = PriceLevel{px, 250 + static_cast<Quantity>(i % 400), 2};
        packets.push_back(encode_wire_quote(u));

        reqs[i].parent_id = i + 1;
        reqs[i].side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        reqs[i].qty = qty_dist(rng);
        reqs[i].limit_price = (reqs[i].side == Side::Buy) ? 1'510'000 : 1'490'000;
        reqs[i].tif = TimeInForce::IOC;
        reqs[i].now_ns = ts + 10'000;
    }

    volatile std::int64_t sink = 0;
    for (std::size_t i = 0; i < warmup; ++i) {
        NormalizedQuoteUpdate norm{};
        if (normalizer.decode_and_normalize(packets[i], norm) == DecodeError::Ok) {
            book.apply_update(norm);
        }
        const auto plan = planner.plan(book, &health, reqs[i]);
        sink += plan.planned_qty;
    }

    constexpr std::size_t kBatch = 16;
    const std::size_t num_batches = samples / kBatch;
    std::vector<double> lat_ns;
    lat_ns.reserve(num_batches);

    const auto t_start = std::chrono::steady_clock::now();
    for (std::size_t b = 0; b < num_batches; ++b) {
        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < kBatch; ++k) {
            const std::size_t idx = warmup + b * kBatch + k;
            NormalizedQuoteUpdate norm{};
            if (normalizer.decode_and_normalize(packets[idx], norm) == DecodeError::Ok) {
                book.apply_update(norm);
            }
            const auto plan = planner.plan(book, &health, reqs[idx]);
            sink += plan.planned_qty;
        }
        const auto b1 = std::chrono::steady_clock::now();
        const double ns =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count()) /
            static_cast<double>(kBatch);
        lat_ns.push_back(ns);
    }
    const auto t_end = std::chrono::steady_clock::now();
    const double wall_sec = std::chrono::duration<double>(t_end - t_start).count();

    auto r = summarize("wire_decode_update_route", num_venues, depth, warmup, lat_ns, wall_sec, sink);
    r.samples = num_batches * kBatch;
    r.throughput_ops_per_sec = wall_sec > 0.0 ? (static_cast<double>(r.samples) / wall_sec) : 0.0;
    return r;
}

void print_json(const std::vector<BenchResult>& results) {
    std::cout << std::fixed << std::setprecision(2) << "[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "  {\"name\":\"" << r.name << "\","
                  << "\"venues\":" << r.venues << ","
                  << "\"depth\":" << r.depth << ","
                  << "\"warmup\":" << r.warmup << ","
                  << "\"samples\":" << r.samples << ","
                  << "\"throughput_ops_per_sec\":" << r.throughput_ops_per_sec << ","
                  << "\"mean_ns\":" << r.mean_ns << ","
                  << "\"p50_ns\":" << r.p50_ns << ","
                  << "\"p90_ns\":" << r.p90_ns << ","
                  << "\"p99_ns\":" << r.p99_ns << ","
                  << "\"p999_ns\":" << r.p999_ns << ","
                  << "\"max_ns\":" << r.max_ns << "}"
                  << (i + 1 < results.size() ? "," : "") << "\n";
    }
    std::cout << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t warmup = 20'000;
    std::size_t samples = 200'000;
    std::size_t depth = 5;
    std::uint64_t seed = 42;
    bool json_mode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--warmup" && i + 1 < argc) {
            warmup = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--samples" && i + 1 < argc) {
            samples = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--depth" && i + 1 < argc) {
            depth = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--json") {
            json_mode = true;
        }
    }

    std::vector<BenchResult> results;
    for (std::size_t v : {std::size_t{3}, std::size_t{5}, std::size_t{8}}) {
        results.push_back(bench_route_plan(v, depth, warmup, samples, seed));
    }
    results.push_back(bench_wire_update_and_route(3, depth, warmup, samples, seed));

    if (json_mode) {
        print_json(results);
        return 0;
    }

    std::cout << std::left << std::setw(26) << "Benchmark"
              << std::right << std::setw(7) << "Venues"
              << std::setw(7) << "Depth"
              << std::setw(12) << "Ops/sec"
              << std::setw(10) << "p50(ns)"
              << std::setw(10) << "p99(ns)"
              << std::setw(11) << "p99.9(ns)"
              << std::setw(10) << "max(ns)\n";
    std::cout << std::string(93, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(26) << r.name
                  << std::right << std::setw(7) << r.venues
                  << std::setw(7) << r.depth
                  << std::fixed << std::setprecision(0) << std::setw(12) << r.throughput_ops_per_sec
                  << std::setprecision(1) << std::setw(10) << r.p50_ns
                  << std::setw(10) << r.p99_ns
                  << std::setw(11) << r.p999_ns
                  << std::setw(10) << r.max_ns << "\n";
    }
    return 0;
}
