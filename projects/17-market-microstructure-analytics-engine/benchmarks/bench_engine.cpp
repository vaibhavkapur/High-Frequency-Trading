#include "mmae/feature_engine.hpp"
#include "mmae/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BenchMetrics {
    std::string variant;
    size_t warmup_events = 0;
    size_t measured_events = 0;
    double elapsed_ms = 0.0;
    double throughput_eps = 0.0;
    double p50_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    double mean_ns = 0.0;
    uint64_t accepted_events = 0;
    uint64_t total_trades = 0;
    double checksum_ofi = 0.0;
    int64_t checksum_signed_qty = 0;
};

std::vector<mmae::Event> generate_deterministic_workload(size_t n_events, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<mmae::Event> events;
    events.reserve(n_events);

    int64_t ts = 1'700'000'000'000'000'000LL;
    uint64_t seq = 1;
    uint64_t next_oid = 1;
    int64_t mid_ticks = 10000;

    std::vector<uint64_t> active_orders;
    active_orders.reserve(4096);

    // Seed initial 10 bid and 10 ask levels with 4 orders each.
    for (int lvl = 1; lvl <= 10 && events.size() < n_events; ++lvl) {
        for (int k = 0; k < 4 && events.size() < n_events; ++k) {
            mmae::Event eb{};
            eb.event_ts_ns = ts;
            eb.recv_ts_ns = ts + 500;
            eb.seq_num = seq++;
            eb.symbol = "SYNTH";
            eb.type = mmae::EventType::L3_ADD;
            eb.side = mmae::Side::BID;
            eb.price_ticks = mid_ticks - lvl;
            eb.qty = 100 + static_cast<int64_t>(rng() % 500);
            eb.order_id = next_oid++;
            active_orders.push_back(eb.order_id);
            events.push_back(eb);
            ts += 1000;

            if (events.size() >= n_events) break;

            mmae::Event ea{};
            ea.event_ts_ns = ts;
            ea.recv_ts_ns = ts + 500;
            ea.seq_num = seq++;
            ea.symbol = "SYNTH";
            ea.type = mmae::EventType::L3_ADD;
            ea.side = mmae::Side::ASK;
            ea.price_ticks = mid_ticks + lvl;
            ea.qty = 100 + static_cast<int64_t>(rng() % 500);
            ea.order_id = next_oid++;
            active_orders.push_back(ea.order_id);
            events.push_back(ea);
            ts += 1000;
        }
    }

    while (events.size() < n_events) {
        ts += 2000 + static_cast<int64_t>(rng() % 20000);
        uint64_t r = rng() % 100;

        if (r < 42 || active_orders.size() < 60) {
            // L3_ADD
            bool is_bid = (rng() % 2 == 0);
            int64_t offset = 1 + static_cast<int64_t>(rng() % 12);
            mmae::Event ev{};
            ev.event_ts_ns = ts;
            ev.recv_ts_ns = ts + 400;
            ev.seq_num = seq++;
            ev.symbol = "SYNTH";
            ev.type = mmae::EventType::L3_ADD;
            ev.side = is_bid ? mmae::Side::BID : mmae::Side::ASK;
            ev.price_ticks = is_bid ? (mid_ticks - offset) : (mid_ticks + offset);
            ev.qty = 50 + static_cast<int64_t>(rng() % 450);
            ev.order_id = next_oid++;
            active_orders.push_back(ev.order_id);
            events.push_back(ev);
        } else if (r < 68) {
            // L3_CANCEL (full cancel of random active order)
            size_t idx = rng() % active_orders.size();
            uint64_t oid = active_orders[idx];
            active_orders[idx] = active_orders.back();
            active_orders.pop_back();

            mmae::Event ev{};
            ev.event_ts_ns = ts;
            ev.recv_ts_ns = ts + 400;
            ev.seq_num = seq++;
            ev.symbol = "SYNTH";
            ev.type = mmae::EventType::L3_CANCEL;
            ev.side = mmae::Side::UNKNOWN;
            ev.price_ticks = 0;
            ev.qty = 0;
            ev.order_id = oid;
            events.push_back(ev);
        } else if (r < 80) {
            // L3_MODIFY
            size_t idx = rng() % active_orders.size();
            uint64_t oid = active_orders[idx];
            int64_t new_px = mid_ticks + static_cast<int64_t>(rng() % 11) - 5;
            if (new_px <= 10) new_px = 100;

            mmae::Event ev{};
            ev.event_ts_ns = ts;
            ev.recv_ts_ns = ts + 400;
            ev.seq_num = seq++;
            ev.symbol = "SYNTH";
            ev.type = mmae::EventType::L3_MODIFY;
            ev.side = mmae::Side::UNKNOWN;
            ev.price_ticks = new_px;
            ev.qty = 50 + static_cast<int64_t>(rng() % 300);
            ev.order_id = oid;
            events.push_back(ev);
        } else if (r < 88) {
            // L3_EXECUTE
            size_t idx = rng() % active_orders.size();
            uint64_t oid = active_orders[idx];
            active_orders[idx] = active_orders.back();
            active_orders.pop_back();

            mmae::Event ev{};
            ev.event_ts_ns = ts;
            ev.recv_ts_ns = ts + 400;
            ev.seq_num = seq++;
            ev.symbol = "SYNTH";
            ev.type = mmae::EventType::L3_EXECUTE;
            ev.side = mmae::Side::UNKNOWN;
            ev.price_ticks = 0;
            ev.qty = 10000;  // full fill
            ev.order_id = oid;
            events.push_back(ev);
        } else {
            // TRADE
            int64_t trade_offset = static_cast<int64_t>(rng() % 3) - 1;  // -1, 0, +1 around mid
            mmae::Event ev{};
            ev.event_ts_ns = ts;
            ev.recv_ts_ns = ts + 400;
            ev.seq_num = seq++;
            ev.symbol = "SYNTH";
            ev.type = mmae::EventType::TRADE;
            ev.side = mmae::Side::UNKNOWN;
            ev.price_ticks = mid_ticks + trade_offset;
            ev.qty = 25 + static_cast<int64_t>(rng() % 200);
            ev.order_id = 0;
            events.push_back(ev);

            if ((rng() % 20) == 0) {
                mid_ticks += (rng() % 2 == 0) ? 1 : -1;
            }
        }
    }
    return events;
}

double percentile_sorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

BenchMetrics run_variant(
    const std::string& variant,
    const std::vector<mmae::Event>& events,
    size_t warmup_events
) {
    mmae::EngineConfig cfg{};
    cfg.variant = variant;
    cfg.max_levels = 2048;
    cfg.max_orders = 131072;
    cfg.depth_levels_k = 5;
    cfg.tick_size = 0.01;
    cfg.stale_threshold_ns = 10'000'000'000LL;
    cfg.flow_window_ns = 1'000'000'000LL;

    mmae::FeatureEngine engine(cfg);
    size_t measured_n = events.size() > warmup_events ? (events.size() - warmup_events) : 0;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(measured_n);

    double checksum_ofi = 0.0;
    int64_t checksum_signed = 0;

    // Warm-up phase (not included in measured latency/throughput).
    for (size_t i = 0; i < warmup_events && i < events.size(); ++i) {
        auto r = engine.process_event(events[i], i + 2);
        if (r.accepted) {
            checksum_ofi += r.feature.ofi_event;
            checksum_signed += r.feature.signed_qty;
        }
    }

    auto t_start = std::chrono::steady_clock::now();
    for (size_t i = warmup_events; i < events.size(); ++i) {
        auto e0 = std::chrono::steady_clock::now();
        auto r = engine.process_event(events[i], i + 2);
        auto e1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(e1 - e0).count();
        latencies_ns.push_back(ns);
        if (r.accepted) {
            checksum_ofi += r.feature.ofi_event;
            checksum_signed += r.feature.signed_qty;
        }
    }
    auto t_end = std::chrono::steady_clock::now();

    double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double sum_ns = 0.0;
    for (double x : latencies_ns) sum_ns += x;
    std::sort(latencies_ns.begin(), latencies_ns.end());

    BenchMetrics m{};
    m.variant = variant;
    m.warmup_events = warmup_events;
    m.measured_events = measured_n;
    m.elapsed_ms = elapsed_ms;
    m.throughput_eps = (elapsed_ms > 0.0) ? (static_cast<double>(measured_n) / (elapsed_ms / 1000.0)) : 0.0;
    m.p50_ns = percentile_sorted(latencies_ns, 0.50);
    m.p95_ns = percentile_sorted(latencies_ns, 0.95);
    m.p99_ns = percentile_sorted(latencies_ns, 0.99);
    m.p999_ns = percentile_sorted(latencies_ns, 0.999);
    m.mean_ns = latencies_ns.empty() ? 0.0 : (sum_ns / static_cast<double>(latencies_ns.size()));
    m.accepted_events = engine.diagnostics().accepted_events;
    m.total_trades = engine.diagnostics().total_trades;
    m.checksum_ofi = checksum_ofi;
    m.checksum_signed_qty = checksum_signed;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    size_t total_events = 120000;
    size_t warmup_events = 20000;
    uint64_t seed = 20261127ULL;
    std::string out_json;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) {
            total_events = std::stoull(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup_events = std::stoull(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if (arg == "--output-json" && i + 1 < argc) {
            out_json = argv[++i];
        }
    }

    auto workload = generate_deterministic_workload(total_events, seed);
    auto base = run_variant("baseline", workload, warmup_events);
    auto opt = run_variant("optimized", workload, warmup_events);

    bool parity_ok = (base.accepted_events == opt.accepted_events) &&
                     (base.total_trades == opt.total_trades) &&
                     (base.checksum_signed_qty == opt.checksum_signed_qty) &&
                     (std::fabs(base.checksum_ofi - opt.checksum_ofi) < 1e-6);

    double speedup = (base.throughput_eps > 0.0) ? (opt.throughput_eps / base.throughput_eps) : 0.0;

    std::ostringstream js;
    js << std::fixed << std::setprecision(3)
       << "{\n"
       << "  \"schema_version\": \"" << mmae::kSchemaVersion << "\",\n"
       << "  \"clock_source\": \"std::chrono::steady_clock\",\n"
       << "  \"compiler\": \"" << __VERSION__ << "\",\n"
       << "  \"cpp_standard\": " << __cplusplus << ",\n"
       << "  \"seed\": " << seed << ",\n"
       << "  \"total_events\": " << total_events << ",\n"
       << "  \"warmup_events\": " << warmup_events << ",\n"
       << "  \"measured_events\": " << base.measured_events << ",\n"
       << "  \"logical_parity_verified\": " << (parity_ok ? "true" : "false") << ",\n"
       << "  \"speedup_optimized_vs_baseline\": " << speedup << ",\n"
       << "  \"variants\": {\n"
       << "    \"baseline\": {\n"
       << "      \"elapsed_ms\": " << base.elapsed_ms << ",\n"
       << "      \"throughput_events_per_sec\": " << base.throughput_eps << ",\n"
       << "      \"latency_ns\": {\n"
       << "        \"mean\": " << base.mean_ns << ",\n"
       << "        \"p50\": " << base.p50_ns << ",\n"
       << "        \"p95\": " << base.p95_ns << ",\n"
       << "        \"p99\": " << base.p99_ns << ",\n"
       << "        \"p999\": " << base.p999_ns << "\n"
       << "      },\n"
       << "      \"accepted_events\": " << base.accepted_events << ",\n"
       << "      \"total_trades\": " << base.total_trades << ",\n"
       << "      \"checksum_ofi\": " << base.checksum_ofi << ",\n"
       << "      \"checksum_signed_qty\": " << base.checksum_signed_qty << "\n"
       << "    },\n"
       << "    \"optimized\": {\n"
       << "      \"elapsed_ms\": " << opt.elapsed_ms << ",\n"
       << "      \"throughput_events_per_sec\": " << opt.throughput_eps << ",\n"
       << "      \"latency_ns\": {\n"
       << "        \"mean\": " << opt.mean_ns << ",\n"
       << "        \"p50\": " << opt.p50_ns << ",\n"
       << "        \"p95\": " << opt.p95_ns << ",\n"
       << "        \"p99\": " << opt.p99_ns << ",\n"
       << "        \"p999\": " << opt.p999_ns << "\n"
       << "      },\n"
       << "      \"accepted_events\": " << opt.accepted_events << ",\n"
       << "      \"total_trades\": " << opt.total_trades << ",\n"
       << "      \"checksum_ofi\": " << opt.checksum_ofi << ",\n"
       << "      \"checksum_signed_qty\": " << opt.checksum_signed_qty << "\n"
       << "    }\n"
       << "  }\n"
       << "}\n";

    if (!out_json.empty()) {
        std::ofstream ofs(out_json);
        if (!ofs.is_open()) {
            std::cerr << "[ERROR] Cannot write benchmark JSON to " << out_json << "\n";
            return 2;
        }
        ofs << js.str();
    }
    std::cout << js.str();
    return parity_ok ? 0 : 1;
}
