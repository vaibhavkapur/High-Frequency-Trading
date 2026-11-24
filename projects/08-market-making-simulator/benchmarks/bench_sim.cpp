#include "mmsim/config.hpp"
#include "mmsim/engine.hpp"
#include "mmsim/features.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/strategy.hpp"
#include "mmsim/synth.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchStats {
    std::string name;
    size_t events = 0;
    double elapsed_sec = 0.0;
    double throughput_ev_per_sec = 0.0;
    double p50_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    double mean_ns = 0.0;
};

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<size_t>(idx);
    const auto hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

// Measures full end-to-end replay throughput and batched per-event latency (batch=64 to stay above
// steady_clock resolution on arm64/x86).
BenchStats bench_full_replay(const std::string& label, const std::vector<mmsim::MarketEvent>& events,
                             mmsim::BookImplType book_impl, mmsim::StrategyType strat_type,
                             int warmup_runs, int measured_runs) {
    mmsim::SimConfig cfg{};
    cfg.book_impl = book_impl;
    cfg.strategy.type = strat_type;

    mmsim::SimulatorEngine engine(cfg);
    volatile double sink = 0.0;

    for (int w = 0; w < warmup_runs; ++w) {
        const auto res = engine.run(events, false);
        sink += res.summary.net_mtm_pnl_dollars;
    }

    double total_sec = 0.0;
    for (int r = 0; r < measured_runs; ++r) {
        const auto t0 = Clock::now();
        const auto res = engine.run(events, false);
        const auto t1 = Clock::now();
        total_sec += std::chrono::duration<double>(t1 - t0).count();
        sink += res.summary.net_mtm_pnl_dollars;
    }

    // Measure per-event hot-path latency distribution (book update + feature extraction + quoting)
    // in batches of 32 events to avoid timer quantization artifacts.
    auto book = mmsim::make_book(book_impl);
    mmsim::FeatureEngine feat(cfg.features);
    auto strat = mmsim::make_strategy(cfg.strategy);
    mmsim::StrategyState st{120, cfg.risk.max_abs_position, cfg.fees.maker_fee_bps};

    constexpr size_t kBatch = 32;
    std::vector<double> lat_samples;
    lat_samples.reserve(events.size() / kBatch + 1);

    // Warmup hot path.
    for (const auto& ev : events) {
        if (ev.type == mmsim::MarketEventType::BookDelta) {
            book->apply_delta(ev.side, ev.price_ticks, ev.qty);
        } else if (ev.type == mmsim::MarketEventType::BookClear) {
            book->clear();
        }
        const auto sig = feat.on_event(ev, *book);
        const auto q = strat->compute_quotes(sig, st);
        sink += static_cast<double>(q.bid_price_ticks + q.ask_price_ticks);
    }

    book->clear();
    feat.reset();
    for (size_t i = 0; i + kBatch <= events.size(); i += kBatch) {
        const auto t0 = Clock::now();
        for (size_t j = 0; j < kBatch; ++j) {
            const auto& ev = events[i + j];
            if (ev.type == mmsim::MarketEventType::BookDelta) {
                book->apply_delta(ev.side, ev.price_ticks, ev.qty);
            } else if (ev.type == mmsim::MarketEventType::BookClear) {
                book->clear();
            }
            const auto sig = feat.on_event(ev, *book);
            const auto q = strat->compute_quotes(sig, st);
            sink += static_cast<double>(q.bid_price_ticks + q.ask_price_ticks);
        }
        const auto t1 = Clock::now();
        const double ns_per_ev =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kBatch);
        lat_samples.push_back(ns_per_ev);
    }
    (void)sink;

    BenchStats bs{};
    bs.name = label;
    bs.events = events.size();
    bs.elapsed_sec = total_sec / static_cast<double>(measured_runs);
    bs.throughput_ev_per_sec = static_cast<double>(events.size()) / bs.elapsed_sec;
    bs.p50_ns = percentile(lat_samples, 0.50);
    bs.p99_ns = percentile(lat_samples, 0.99);
    bs.p999_ns = percentile(lat_samples, 0.999);
    bs.mean_ns =
        std::accumulate(lat_samples.begin(), lat_samples.end(), 0.0) /
        static_cast<double>(std::max<size_t>(1, lat_samples.size()));
    return bs;
}

} // namespace

int main(int argc, char** argv) {
    std::string csv_out;
    double duration_sec = 120.0;
    int measured_runs = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out-csv" && i + 1 < argc) {
            csv_out = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::stod(argv[++i]);
        } else if (arg == "--runs" && i + 1 < argc) {
            measured_runs = std::stoi(argv[++i]);
        }
    }

    mmsim::SynthParams sp{};
    sp.seed = 202601;
    sp.regime = mmsim::MarketRegime::HighVolBurst;
    sp.duration_sec = duration_sec;
    sp.base_event_rate_hz = 1000.0;
    const auto events = mmsim::generate_synthetic_session(sp);

    std::cout << "Benchmark dataset: " << events.size() << " events (" << duration_sec
              << "s simulated high_vol_burst, seed=" << sp.seed << ", runs=" << measured_runs
              << ")\n\n";

    std::vector<BenchStats> rows;
    rows.push_back(bench_full_replay("flat_array + inventory_aware", events,
                                     mmsim::BookImplType::FlatArray,
                                     mmsim::StrategyType::InventoryAware, 2, measured_runs));
    rows.push_back(bench_full_replay("std_map + inventory_aware", events,
                                     mmsim::BookImplType::StdMap,
                                     mmsim::StrategyType::InventoryAware, 2, measured_runs));
    rows.push_back(bench_full_replay("flat_array + naive_symmetric", events,
                                     mmsim::BookImplType::FlatArray,
                                     mmsim::StrategyType::NaiveSymmetric, 2, measured_runs));
    rows.push_back(bench_full_replay("std_map + naive_symmetric", events,
                                     mmsim::BookImplType::StdMap,
                                     mmsim::StrategyType::NaiveSymmetric, 2, measured_runs));

    std::cout << std::left << std::setw(32) << "Configuration" << std::right << std::setw(14)
              << "Throughput" << std::setw(11) << "Mean(ns)" << std::setw(11) << "p50(ns)"
              << std::setw(11) << "p99(ns)" << std::setw(11) << "p99.9(ns)" << "\n";
    std::cout << std::string(90, '-') << "\n";

    for (const auto& r : rows) {
        std::cout << std::left << std::setw(32) << r.name << std::right << std::fixed
                  << std::setprecision(2) << std::setw(11) << (r.throughput_ev_per_sec / 1e6)
                  << " M/s" << std::setprecision(1) << std::setw(11) << r.mean_ns << std::setw(11)
                  << r.p50_ns << std::setw(11) << r.p99_ns << std::setw(11) << r.p999_ns << "\n";
    }

    if (!csv_out.empty()) {
        std::ofstream ofs(csv_out, std::ios::trunc);
        ofs << "config,events,elapsed_sec,throughput_ev_per_sec,mean_ns,p50_ns,p99_ns,p999_ns\n";
        ofs << std::fixed << std::setprecision(4);
        for (const auto& r : rows) {
            ofs << r.name << ',' << r.events << ',' << r.elapsed_sec << ','
                << r.throughput_ev_per_sec << ',' << r.mean_ns << ',' << r.p50_ns << ','
                << r.p99_ns << ',' << r.p999_ns << '\n';
        }
    }
    return 0;
}
