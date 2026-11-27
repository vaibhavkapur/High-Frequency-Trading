#include <cstdint>
#include <fstream>
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

// Generates a strictly legal seeded stream (every event commits with STATUS_OK)
std::vector<EventPacket> generate_legal_stream(const Config& cfg, size_t n_events, uint32_t seed) {
    std::mt19937 rng(seed);
    std::map<uint32_t, uint32_t> bids;
    std::map<uint32_t, uint32_t> asks;

    std::vector<EventPacket> stream;
    stream.reserve(n_events);
    uint32_t ts = 1000;

    for (size_t i = 0; i < n_events; ++i) {
        EventPacket ev{};
        ev.seq_no       = static_cast<uint32_t>(i + 1);
        ev.timestamp_ns = ts;
        ts += 4;
        ev.schema_ver = kSchemaVersion;

        uint32_t roll = rng() % 100u;
        if (roll < 3u) {
            ev.opcode = static_cast<uint8_t>(Opcode::Nop);
            ev.side   = 0;
            ev.price  = 0;
            ev.qty    = 0;
            stream.push_back(ev);
            continue;
        }

        uint8_t side = static_cast<uint8_t>(rng() & 1u);
        auto& book   = (side == 0) ? bids : asks;
        ev.side      = side;

        bool do_remove = !book.empty() && (roll >= 55u);
        if (do_remove) {
            auto it = book.begin();
            std::advance(it, rng() % book.size());
            uint32_t price = it->first;
            uint32_t avail = it->second;
            uint32_t qty   = ((rng() % 3u) == 0u) ? avail : (1u + (rng() % avail));
            ev.price       = price;
            ev.qty         = qty;
            ev.opcode      = static_cast<uint8_t>((rng() & 1u) ? Opcode::Cancel : Opcode::Execute);
            if (qty == avail) book.erase(it);
            else it->second -= qty;
        } else {
            uint32_t price = 0;
            if (book.size() >= cfg.max_active_levels) {
                auto it = book.begin();
                std::advance(it, rng() % book.size());
                price = it->first;
            } else {
                price = cfg.price_base + static_cast<uint32_t>(rng() % cfg.num_levels);
            }
            uint32_t qty = 1u + static_cast<uint32_t>(rng() % 500u);
            // Guard against accidental overflow on legal stream
            if (book.count(price) && static_cast<uint64_t>(book[price]) + qty > 1000000000ULL) {
                qty = 1u;
            }
            ev.price  = price;
            ev.qty    = qty;
            ev.opcode = static_cast<uint8_t>(Opcode::Add);
            book[price] += qty;
        }

        stream.push_back(ev);
    }
    return stream;
}

struct DiffRunStats {
    size_t      events            = 0;
    size_t      mismatches        = 0;
    size_t      unexpected_errors = 0;
    uint64_t    cycles            = 0;
    uint64_t    stalls            = 0;
    uint64_t    resets            = 0;
    Diagnostics diag              = {};
};

DiffRunStats run_legal_1lane(const Config& cfg, const std::vector<EventPacket>& stream) {
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::vector<BboOutputPacket> expected;
    std::vector<BboOutputPacket> observed;
    expected.reserve(stream.size());
    observed.reserve(stream.size());

    size_t idx = 0;
    while (idx < stream.size() || observed.size() < expected.size()) {
        if (dut.out_valid()) observed.push_back(dut.out_bbo());
        bool send = idx < stream.size();
        auto res  = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send,
            .in_event_0 = send ? stream[idx] : EventPacket{},
            .in_valid_1 = false,
            .out_ready  = true,
        });
        if (res.accepted) {
            expected.push_back(ref.apply_single(stream[idx]));
            ++idx;
        }
    }

    DiffRunStats st{};
    st.events = stream.size();
    st.cycles = dut.diagnostics().cycles_total;
    st.diag   = dut.diagnostics();
    if (observed.size() != expected.size()) {
        st.mismatches = std::max(observed.size(), expected.size());
        return st;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!(observed[i] == expected[i])) ++st.mismatches;
        if (observed[i].status_0 != Status::Ok) ++st.unexpected_errors;
    }
    return st;
}

DiffRunStats run_legal_2lane(const Config& cfg, const std::vector<EventPacket>& stream) {
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::vector<BboOutputPacket> expected;
    std::vector<BboOutputPacket> observed;

    size_t idx = 0;
    while (idx < stream.size() || observed.size() < expected.size()) {
        if (dut.out_valid()) observed.push_back(dut.out_bbo());
        bool send0 = idx < stream.size();
        bool send1 = (idx + 1) < stream.size();
        auto res   = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send0,
            .in_event_0 = send0 ? stream[idx] : EventPacket{},
            .in_valid_1 = send1,
            .in_event_1 = send1 ? stream[idx + 1] : EventPacket{},
            .out_ready  = true,
        });
        if (res.accepted) {
            if (send1) {
                expected.push_back(ref.apply_pair(stream[idx], stream[idx + 1]));
                idx += 2;
            } else {
                expected.push_back(ref.apply_single(stream[idx]));
                idx += 1;
            }
        }
    }

    DiffRunStats st{};
    st.events = stream.size();
    st.cycles = dut.diagnostics().cycles_total;
    st.diag   = dut.diagnostics();
    if (observed.size() != expected.size()) {
        st.mismatches = std::max(observed.size(), expected.size());
        return st;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!(observed[i] == expected[i])) ++st.mismatches;
        if (observed[i].status_0 != Status::Ok || (observed[i].lane1_valid && observed[i].status_1 != Status::Ok)) {
            ++st.unexpected_errors;
        }
    }
    return st;
}

// Mixed adversarial stream: legal + illegal + boundary + random backpressure + dual-lane + mid-stream resets
DiffRunStats run_mixed_adversarial(const Config& cfg, size_t n_events, uint32_t seed) {
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::mt19937 rng(seed ^ 0x5A5A5A5Au);
    DiffRunStats st{};
    st.events = n_events;

    std::vector<BboOutputPacket> expected;
    std::vector<BboOutputPacket> observed;

    auto make_random_event = [&](uint32_t seq) {
        EventPacket ev{};
        ev.seq_no       = seq;
        ev.timestamp_ns = seq * 4;
        ev.schema_ver   = ((rng() % 50) == 0) ? 2 : kSchemaVersion;
        ev.flags        = ((rng() % 50) == 0) ? 1 : 0;
        uint32_t op_r   = rng() % 20;
        ev.opcode       = static_cast<uint8_t>(op_r == 0 ? 7 : (op_r % 4));
        ev.side         = static_cast<uint8_t>((rng() % 40 == 0) ? 3 : (rng() & 1u));
        uint32_t p_r    = rng() % 30;
        if (p_r == 0) ev.price = cfg.price_base - 1;
        else if (p_r == 1) ev.price = cfg.price_base + cfg.num_levels;
        else ev.price = cfg.price_base + static_cast<uint32_t>(rng() % cfg.num_levels);
        uint32_t q_r = rng() % 30;
        if (q_r == 0) ev.qty = 0;
        else if (q_r == 1) ev.qty = 0xFFFFFFF0u;
        else ev.qty = 1u + static_cast<uint32_t>(rng() % 250u);
        return ev;
    };

    size_t sent = 0;
    uint32_t seq = 1;
    uint64_t total_cycles_acc = 0;
    uint64_t total_stalls_acc = 0;

    while (sent < n_events) {
        // Trigger clean reset epoch every 2500 events after draining
        if (sent > 0 && (sent % 2500) == 0) {
            while (observed.size() < expected.size()) {
                if (dut.out_valid()) observed.push_back(dut.out_bbo());
                dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
            }
            for (size_t i = 0; i < expected.size(); ++i) {
                if (!(observed[i] == expected[i])) ++st.mismatches;
            }
            total_cycles_acc += dut.diagnostics().cycles_total;
            total_stalls_acc += dut.diagnostics().backpressure_stalls;
            expected.clear();
            observed.clear();

            dut.tick(TickInput{.rst_n = false});
            ref.reset();
            ++st.resets;
        }

        bool out_ready = (rng() % 100) >= 20;
        bool dual_lane = ((rng() & 1u) == 1u) && (sent + 1 < n_events) && (((sent + 1) % 2500) != 0);

        if (dut.out_valid() && out_ready) {
            observed.push_back(dut.out_bbo());
        }

        EventPacket e0 = make_random_event(seq);
        EventPacket e1 = dual_lane ? make_random_event(seq + 1) : EventPacket{};

        auto res = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = true,
            .in_event_0 = e0,
            .in_valid_1 = dual_lane,
            .in_event_1 = e1,
            .out_ready  = out_ready,
        });
        if (res.accepted) {
            if (dual_lane) {
                expected.push_back(ref.apply_pair(e0, e1));
                sent += 2;
                seq += 2;
            } else {
                expected.push_back(ref.apply_single(e0));
                sent += 1;
                seq += 1;
            }
        }
    }

    while (observed.size() < expected.size()) {
        if (dut.out_valid()) observed.push_back(dut.out_bbo());
        dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!(observed[i] == expected[i])) ++st.mismatches;
    }

    st.cycles = total_cycles_acc + dut.diagnostics().cycles_total;
    st.stalls = total_stalls_acc + dut.diagnostics().backpressure_stalls;
    st.diag   = dut.diagnostics();
    return st;
}

}  // namespace

int main(int argc, char** argv) {
    size_t n_legal = 25000;
    size_t n_mixed = 10000;
    uint32_t seed  = 20261127;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--events" && i + 1 < argc) n_legal = std::stoull(argv[++i]);
        else if (a == "--mixed" && i + 1 < argc) n_mixed = std::stoull(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    }

    Config cfg{10000, 64, 32};
    auto legal_stream = generate_legal_stream(cfg, n_legal, seed);

    auto r1    = run_legal_1lane(cfg, legal_stream);
    auto r2    = run_legal_2lane(cfg, legal_stream);
    auto mixed = run_mixed_adversarial(cfg, n_mixed, seed);

    bool ok = (r1.mismatches == 0 && r1.unexpected_errors == 0 &&
               r2.mismatches == 0 && r2.unexpected_errors == 0 &&
               mixed.mismatches == 0);

    if (!out_path.empty()) {
        std::ofstream ofs(out_path);
        ofs << "{\n"
            << "  \"schema_version\": \"fpga-ob.v1\",\n"
            << "  \"seed\": " << seed << ",\n"
            << "  \"config\": {\"price_base\": " << cfg.price_base
            << ", \"num_levels\": " << cfg.num_levels
            << ", \"max_active_levels\": " << cfg.max_active_levels << "},\n"
            << "  \"legal_1lane\": {"
            << "\"events\": " << r1.events
            << ", \"mismatches\": " << r1.mismatches
            << ", \"unexpected_errors\": " << r1.unexpected_errors
            << ", \"cycles\": " << r1.cycles
            << ", \"cycles_per_event\": " << (static_cast<double>(r1.cycles) / static_cast<double>(r1.events))
            << ", \"diagnostics\": " << r1.diag.to_json() << "},\n"
            << "  \"legal_2lane\": {"
            << "\"events\": " << r2.events
            << ", \"mismatches\": " << r2.mismatches
            << ", \"unexpected_errors\": " << r2.unexpected_errors
            << ", \"cycles\": " << r2.cycles
            << ", \"cycles_per_event\": " << (static_cast<double>(r2.cycles) / static_cast<double>(r2.events))
            << ", \"diagnostics\": " << r2.diag.to_json() << "},\n"
            << "  \"mixed_adversarial\": {"
            << "\"events\": " << mixed.events
            << ", \"mismatches\": " << mixed.mismatches
            << ", \"cycles\": " << mixed.cycles
            << ", \"backpressure_stalls\": " << mixed.stalls
            << ", \"mid_stream_resets\": " << mixed.resets
            << ", \"last_epoch_diagnostics\": " << mixed.diag.to_json() << "},\n"
            << "  \"passed\": " << (ok ? "true" : "false") << "\n"
            << "}\n";
    }

    if (ok) {
        std::cout << "[PASS] Differential testbench: 0 mismatches across "
                  << r1.events << " legal 1-lane + " << r2.events << " legal 2-lane + "
                  << mixed.events << " mixed adversarial events (seed=" << seed << ").\n";
        return 0;
    }

    std::cerr << "[FAIL] Differential testbench mismatches: "
              << "1lane=" << r1.mismatches << " (err=" << r1.unexpected_errors << "), "
              << "2lane=" << r2.mismatches << " (err=" << r2.unexpected_errors << "), "
              << "mixed=" << mixed.mismatches << "\n";
    return 1;
}
