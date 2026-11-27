#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "fpga_ob/contracts.hpp"
#include "fpga_ob/reference_book.hpp"
#include "fpga_ob/rtl_model.hpp"

using namespace fpga_ob;

namespace {

std::vector<EventPacket> build_workload(const Config& cfg, size_t n_events, uint32_t seed) {
    std::mt19937 rng(seed);
    std::map<uint32_t, uint32_t> bids;
    std::map<uint32_t, uint32_t> asks;

    std::vector<EventPacket> evs;
    evs.reserve(n_events);
    uint32_t ts = 1000;
    for (size_t i = 0; i < n_events; ++i) {
        EventPacket ev{};
        ev.seq_no       = static_cast<uint32_t>(i + 1);
        ev.timestamp_ns = ts;
        ts += 4;
        ev.schema_ver = kSchemaVersion;
        ev.side       = static_cast<uint8_t>(rng() & 1u);
        auto& book    = (ev.side == 0) ? bids : asks;

        if (!book.empty() && (rng() % 100u) >= 55u) {
            auto it = book.begin();
            std::advance(it, rng() % book.size());
            ev.price  = it->first;
            ev.qty    = ((rng() % 3u) == 0u) ? it->second : (1u + rng() % it->second);
            ev.opcode = static_cast<uint8_t>((rng() & 1u) ? Opcode::Cancel : Opcode::Execute);
            if (ev.qty == it->second) book.erase(it);
            else it->second -= ev.qty;
        } else {
            if (book.size() >= cfg.max_active_levels) {
                auto it = book.begin();
                std::advance(it, rng() % book.size());
                ev.price = it->first;
            } else {
                ev.price = cfg.price_base + static_cast<uint32_t>(rng() % cfg.num_levels);
            }
            ev.qty    = 1u + static_cast<uint32_t>(rng() % 500u);
            ev.opcode = static_cast<uint8_t>(Opcode::Add);
            book[ev.price] += ev.qty;
        }
        evs.push_back(ev);
    }
    return evs;
}

struct BenchMetrics {
    std::string variant;
    size_t      events                    = 0;
    uint64_t    total_cycles              = 0;
    uint32_t    first_event_latency_cyc   = 0;
    double      cycles_per_event          = 0.0;
    double      host_sim_ms               = 0.0;
    double      host_events_per_sec       = 0.0;
    double      assumed_250mhz_lat_ns     = 0.0;
    double      assumed_250mhz_meps       = 0.0;
    double      assumed_300mhz_lat_ns     = 0.0;
    double      assumed_300mhz_meps       = 0.0;
    uint32_t    final_best_bid            = 0;
    uint32_t    final_best_ask            = 0;
};

BenchMetrics bench_baseline_fsm(const Config& cfg, const std::vector<EventPacket>& stream) {
    BaselineFsmOrderBook dut(cfg);
    dut.tick(TickInput{.rst_n = false});

    BboOutputPacket last{};
    uint32_t first_lat = 0;
    size_t out_cnt = 0;
    size_t idx = 0;

    auto t0 = std::chrono::steady_clock::now();
    while (idx < stream.size() || out_cnt < stream.size()) {
        bool send = idx < stream.size();
        auto res  = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send,
            .in_event_0 = send ? stream[idx] : EventPacket{},
            .out_ready  = true,
        });
        if (res.accepted) ++idx;
        if (res.out_valid) {
            if (out_cnt == 0) first_lat = static_cast<uint32_t>(dut.diagnostics().cycles_total);
            last = res.out_bbo;
            ++out_cnt;
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    BenchMetrics m{};
    m.variant                 = "baseline_fsm_unpipelined";
    m.events                  = stream.size();
    m.total_cycles            = dut.diagnostics().cycles_total;
    m.first_event_latency_cyc = first_lat;
    m.cycles_per_event        = static_cast<double>(m.total_cycles) / static_cast<double>(m.events);
    m.host_sim_ms             = ms;
    m.host_events_per_sec     = (static_cast<double>(m.events) / ms) * 1000.0;
    m.assumed_250mhz_lat_ns   = static_cast<double>(m.first_event_latency_cyc) * 4.0;
    m.assumed_250mhz_meps     = 250.0 / m.cycles_per_event;
    m.assumed_300mhz_lat_ns   = static_cast<double>(m.first_event_latency_cyc) * (1000.0 / 300.0);
    m.assumed_300mhz_meps     = 300.0 / m.cycles_per_event;
    m.final_best_bid          = last.best_bid_price;
    m.final_best_ask          = last.best_ask_price;
    return m;
}

BenchMetrics bench_pipelined(const Config& cfg, const std::vector<EventPacket>& stream, bool dual_lane) {
    PipelinedOrderBookAccelerator dut(cfg);
    dut.tick(TickInput{.rst_n = false});

    BboOutputPacket last{};
    uint32_t first_lat = 0;
    size_t out_cnt = 0;
    const size_t expected_outputs = dual_lane ? ((stream.size() + 1) / 2) : stream.size();
    size_t idx = 0;

    auto t0 = std::chrono::steady_clock::now();
    while (idx < stream.size() || out_cnt < expected_outputs) {
        if (dut.out_valid()) {
            last = dut.out_bbo();
            ++out_cnt;
        }
        bool send0 = idx < stream.size();
        bool send1 = dual_lane && (idx + 1 < stream.size());
        auto res   = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send0,
            .in_event_0 = send0 ? stream[idx] : EventPacket{},
            .in_valid_1 = send1,
            .in_event_1 = send1 ? stream[idx + 1] : EventPacket{},
            .out_ready  = true,
        });
        if (res.accepted) idx += (send1 ? 2 : 1);
        if (res.out_valid && first_lat == 0) {
            first_lat = static_cast<uint32_t>(dut.diagnostics().cycles_total);
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    BenchMetrics m{};
    m.variant                 = dual_lane ? "pipelined_tree_2lane" : "pipelined_tree_1lane";
    m.events                  = stream.size();
    m.total_cycles            = dut.diagnostics().cycles_total;
    m.first_event_latency_cyc = first_lat;
    m.cycles_per_event        = static_cast<double>(m.total_cycles) / static_cast<double>(m.events);
    m.host_sim_ms             = ms;
    m.host_events_per_sec     = (static_cast<double>(m.events) / ms) * 1000.0;
    m.assumed_250mhz_lat_ns   = static_cast<double>(m.first_event_latency_cyc) * 4.0;
    m.assumed_250mhz_meps     = 250.0 / m.cycles_per_event;
    m.assumed_300mhz_lat_ns   = static_cast<double>(m.first_event_latency_cyc) * (1000.0 / 300.0);
    m.assumed_300mhz_meps     = 300.0 / m.cycles_per_event;
    m.final_best_bid          = last.best_bid_price;
    m.final_best_ask          = last.best_ask_price;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    size_t n_events = 100000;
    size_t n_warmup = 2000;
    uint32_t seed   = 20261127;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--events" && i + 1 < argc) n_events = std::stoull(argv[++i]);
        else if (a == "--warmup" && i + 1 < argc) n_warmup = std::stoull(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    }

    Config cfg{10000, 64, 32};
    auto warmup_stream = build_workload(cfg, n_warmup, seed ^ 1u);
    auto main_stream   = build_workload(cfg, n_events, seed);

    // Warm-up run (discarded)
    (void)bench_pipelined(cfg, warmup_stream, false);

    // Like-for-like benchmark on identical seeded stream
    auto b_fsm = bench_baseline_fsm(cfg, main_stream);
    auto b_p1  = bench_pipelined(cfg, main_stream, false);
    auto b_p2  = bench_pipelined(cfg, main_stream, true);

    // Sanity check: all three variants must converge to the exact same final BBO
    if (b_fsm.final_best_bid != b_p1.final_best_bid || b_fsm.final_best_ask != b_p1.final_best_ask ||
        b_p1.final_best_bid != b_p2.final_best_bid || b_p1.final_best_ask != b_p2.final_best_ask) {
        std::cerr << "[FAIL] Final BBO mismatch across benchmark variants!\n";
        return 1;
    }

    std::vector<BenchMetrics> all = {b_fsm, b_p1, b_p2};

    std::cout << std::fixed << std::setprecision(4);
    for (const auto& m : all) {
        std::cout << m.variant
                  << " | events=" << m.events
                  << " | cycles=" << m.total_cycles
                  << " | cycles/event=" << m.cycles_per_event
                  << " | latency_cycles=" << m.first_event_latency_cyc
                  << " | host_sim_ms=" << m.host_sim_ms
                  << " | assumed_250MHz_Meps=" << m.assumed_250mhz_meps
                  << "\n";
    }

    if (!out_path.empty()) {
        std::ofstream ofs(out_path);
        ofs << std::fixed << std::setprecision(6);
        ofs << "{\n"
            << "  \"schema_version\": \"fpga-ob.v1\",\n"
            << "  \"measurement_type\": \"simulated_cycle_count_and_host_simulation_time\",\n"
            << "  \"physical_hardware_timing_measured\": false,\n"
            << "  \"clock_assumptions_note\": \"250 MHz (4.0 ns) and 300 MHz (3.333 ns) figures are analytical projections from simulated cycle counts, not physical FPGA timing closure.\",\n"
            << "  \"seed\": " << seed << ",\n"
            << "  \"warmup_events\": " << n_warmup << ",\n"
            << "  \"measured_events\": " << n_events << ",\n"
            << "  \"config\": {\"price_base\": " << cfg.price_base
            << ", \"num_levels\": " << cfg.num_levels
            << ", \"max_active_levels\": " << cfg.max_active_levels << "},\n"
            << "  \"variants\": [\n";
        for (size_t i = 0; i < all.size(); ++i) {
            const auto& m = all[i];
            ofs << "    {"
                << "\"variant\":\"" << m.variant << "\","
                << "\"events\":" << m.events << ","
                << "\"total_cycles\":" << m.total_cycles << ","
                << "\"first_event_latency_cycles\":" << m.first_event_latency_cyc << ","
                << "\"cycles_per_event\":" << m.cycles_per_event << ","
                << "\"host_sim_ms\":" << m.host_sim_ms << ","
                << "\"host_events_per_sec\":" << m.host_events_per_sec << ","
                << "\"projected_250mhz_latency_ns\":" << m.assumed_250mhz_lat_ns << ","
                << "\"projected_250mhz_meps\":" << m.assumed_250mhz_meps << ","
                << "\"projected_300mhz_latency_ns\":" << m.assumed_300mhz_lat_ns << ","
                << "\"projected_300mhz_meps\":" << m.assumed_300mhz_meps << ","
                << "\"final_best_bid\":" << m.final_best_bid << ","
                << "\"final_best_ask\":" << m.final_best_ask
                << "}" << (i + 1 < all.size() ? "," : "") << "\n";
        }
        ofs << "  ]\n}\n";
    }

    return 0;
}
