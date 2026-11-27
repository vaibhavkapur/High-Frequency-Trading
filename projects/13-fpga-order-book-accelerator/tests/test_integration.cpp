#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fpga_ob/contracts.hpp"
#include "fpga_ob/reference_book.hpp"
#include "fpga_ob/rtl_model.hpp"

using namespace fpga_ob;

namespace {

int g_failures = 0;

struct ScenarioResult {
    std::string name;
    bool        passed = true;
    size_t      events = 0;
    size_t      mismatches = 0;
    uint64_t    cycles = 0;
    uint64_t    stalls = 0;
    uint64_t    dual_lane_cycles = 0;
};

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    }
}

ScenarioResult test_backpressure_patterns() {
    ScenarioResult sr{"backpressure_periodic_and_random_stalls"};
    Config cfg{10000, 64, 32};
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::mt19937 rng(424242);
    const size_t total = 2500;
    std::vector<EventPacket> stream;
    stream.reserve(total);
    for (size_t i = 0; i < total; ++i) {
        EventPacket ev{};
        ev.seq_no       = static_cast<uint32_t>(i + 1);
        ev.timestamp_ns = static_cast<uint32_t>((i + 1) * 4);
        ev.side         = static_cast<uint8_t>(rng() & 1u);
        ev.price        = cfg.price_base + static_cast<uint32_t>(rng() % 32u);
        ev.qty          = 1u + static_cast<uint32_t>(rng() % 200u);
        uint32_t r      = rng() % 10u;
        ev.opcode       = static_cast<uint8_t>(r < 6 ? Opcode::Add : (r < 8 ? Opcode::Cancel : Opcode::Execute));
        ev.schema_ver   = kSchemaVersion;
        stream.push_back(ev);
    }

    std::vector<BboOutputPacket> expected;
    std::vector<BboOutputPacket> observed;
    expected.reserve(total);
    observed.reserve(total);

    size_t idx = 0;
    uint64_t cycle = 0;
    while (idx < total || observed.size() < expected.size()) {
        // Mix burst stalls (every 200 cycles, stall 15 cycles) + pseudo-random stalls (30% stall rate)
        bool out_ready = true;
        if (idx < total) {
            if ((cycle % 200) >= 100 && (cycle % 200) < 115) {
                out_ready = false;
            } else if ((rng() % 100) < 30) {
                out_ready = false;
            }
        }

        // Standard synchronous AXI-Stream consumer samples BEFORE clock edge overwrites out_reg
        if (dut.out_valid() && out_ready) {
            observed.push_back(dut.out_bbo());
        }

        const bool send = (idx < total);
        auto res = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send,
            .in_event_0 = send ? stream[idx] : EventPacket{},
            .in_valid_1 = false,
            .out_ready  = out_ready,
        });

        if (res.accepted) {
            expected.push_back(ref.apply_single(stream[idx]));
            ++idx;
        }
        ++cycle;
        if (cycle > total * 20) break;
    }

    sr.events = total;
    sr.cycles = dut.diagnostics().cycles_total;
    sr.stalls = dut.diagnostics().backpressure_stalls;
    if (observed.size() != expected.size()) {
        sr.mismatches = std::max(observed.size(), expected.size());
        sr.passed = false;
        check(false, "Backpressure output count mismatch: expected=" + std::to_string(expected.size()) +
                     " observed=" + std::to_string(observed.size()));
    } else {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (!(observed[i] == expected[i])) {
                ++sr.mismatches;
                sr.passed = false;
            }
        }
    }
    check(sr.mismatches == 0, "Backpressure scenario must have 0 mismatches");
    check(sr.stalls > 100, "Backpressure scenario must exercise backpressure stalls");
    return sr;
}

ScenarioResult test_simultaneous_updates() {
    ScenarioResult sr{"simultaneous_dual_lane_updates_and_collisions"};
    Config cfg{10000, 16, 2};  // Tight capacity (2 active levels) to test simultaneous capacity race!
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::vector<std::pair<EventPacket, EventPacket>> pairs = {
        // 1. Opposite sides simultaneous (Bid + Ask)
        {{1, 10, 10005, 100, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {2, 10, 10008, 150, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Ask), 0, kSchemaVersion}},

        // 2. Same side, SAME price collision: ADD +50 then CANCEL -30 in same clock cycle
        {{3, 20, 10005, 50, static_cast<uint8_t>(Opcode::Add),    static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {4, 20, 10005, 30, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion}},

        // 3. Same side, distinct price fills capacity slot #2 (10006), Lane 1 tries slot #3 (10007 -> ERR_FULL_CAPACITY)
        {{5, 30, 10006, 80, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {6, 30, 10007, 40, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion}},

        // 4. Same side: Lane 0 cancels 10006 completely (frees slot!), Lane 1 immediately adds 10007 (MUST SUCCEED!)
        {{7, 40, 10006, 80, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {8, 40, 10007, 40, static_cast<uint8_t>(Opcode::Add),    static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion}},

        // 5. Same side, SAME price: Lane 0 executes 10007 completely -> Lane 1 tries to cancel 10007 -> ERR_INVALID_CANCEL
        {{9,  50, 10007, 40, static_cast<uint8_t>(Opcode::Execute), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {10, 50, 10007, 1,  static_cast<uint8_t>(Opcode::Cancel),  static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion}},

        // 6. Same side, SAME price: Lane 0 invalid cancel (no state change), Lane 1 valid ADD -> must succeed
        {{11, 60, 10009, 10, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Ask), 0, kSchemaVersion},
         {12, 60, 10009, 60, static_cast<uint8_t>(Opcode::Add),    static_cast<uint8_t>(Side::Ask), 0, kSchemaVersion}},

        // 7. Simultaneous ADD + EXECUTE on empty level -> ADD activates it, EXECUTE empties it in 1 cycle!
        {{13, 70, 10012, 90, static_cast<uint8_t>(Opcode::Add),     static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
         {14, 70, 10012, 90, static_cast<uint8_t>(Opcode::Execute), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion}},
    };

    std::vector<BboOutputPacket> expected;
    std::vector<BboOutputPacket> observed;

    // Drive with interleaved backpressure stalls as well
    size_t idx = 0;
    uint64_t cyc = 0;
    while (idx < pairs.size() || observed.size() < expected.size()) {
        bool out_ready = ((cyc % 3) != 1);  // 33% periodic backpressure
        if (dut.out_valid() && out_ready) {
            observed.push_back(dut.out_bbo());
        }
        bool send = idx < pairs.size();
        auto res = dut.tick(TickInput{
            .rst_n      = true,
            .in_valid_0 = send,
            .in_event_0 = send ? pairs[idx].first : EventPacket{},
            .in_valid_1 = send,
            .in_event_1 = send ? pairs[idx].second : EventPacket{},
            .out_ready  = out_ready,
        });
        if (res.accepted) {
            expected.push_back(ref.apply_pair(pairs[idx].first, pairs[idx].second));
            ++idx;
        }
        ++cyc;
        if (cyc > 200) break;
    }

    sr.events           = pairs.size() * 2;
    sr.cycles           = dut.diagnostics().cycles_total;
    sr.stalls           = dut.diagnostics().backpressure_stalls;
    sr.dual_lane_cycles = dut.diagnostics().dual_lane_cycles;

    check(observed.size() == expected.size(), "Simultaneous update output count mismatch");
    for (size_t i = 0; i < expected.size() && i < observed.size(); ++i) {
        if (!(observed[i] == expected[i])) {
            ++sr.mismatches;
            sr.passed = false;
            std::cerr << "[FAIL] Simultaneous pair " << i << "\n"
                      << "  expected: " << to_json(expected[i]) << "\n"
                      << "  observed: " << to_json(observed[i]) << "\n";
        }
    }
    check(observed.size() >= 7 && observed[2].status_1 == Status::ErrFullCapacity,
          "Pair 3 Lane 1 must hit ERR_FULL_CAPACITY");
    check(observed.size() >= 7 && observed[3].status_1 == Status::Ok,
          "Pair 4 Lane 1 must succeed after Lane 0 frees capacity slot");
    check(observed.size() >= 7 && observed[4].status_1 == Status::ErrInvalidCancel,
          "Pair 5 Lane 1 must hit ERR_INVALID_CANCEL after Lane 0 empties level");
    check(sr.dual_lane_cycles == pairs.size(), "dual_lane_cycles counter must match pair count");
    return sr;
}

ScenarioResult test_binary_replay_reproducibility() {
    ScenarioResult sr{"binary_stream_replay_reproducibility"};
    auto bundle = load_events_json("tests/fixtures/deterministic_basic.json");
    const std::string tmp_bin = "build/replay_test.bin";
    write_binary_stream(tmp_bin, bundle.events);
    auto replayed = read_binary_stream(tmp_bin);
    check(replayed == bundle.events, "Binary stream round-trip must match original events");

    auto run_once = [&](const std::vector<EventPacket>& evs) {
        PipelinedOrderBookAccelerator dut(bundle.config);
        dut.tick(TickInput{.rst_n = false});
        std::vector<BboOutputPacket> outs;
        for (const auto& e : evs) {
            if (dut.out_valid()) outs.push_back(dut.out_bbo());
            dut.tick(TickInput{.rst_n = true, .in_valid_0 = true, .in_event_0 = e, .out_ready = true});
        }
        for (int i = 0; i < 6; ++i) {
            if (dut.out_valid()) outs.push_back(dut.out_bbo());
            dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
        }
        return std::make_pair(outs, dut.diagnostics());
    };

    auto [out1, diag1] = run_once(replayed);
    auto [out2, diag2] = run_once(replayed);
    sr.events = replayed.size();
    sr.cycles = diag1.cycles_total;
    if (out1 != out2 || diag1.to_json() != diag2.to_json()) {
        sr.mismatches = 1;
        sr.passed     = false;
    }
    check(sr.passed, "Two replays of identical binary stream must be bit-identical");
    return sr;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        }
    }

    std::vector<ScenarioResult> results;
    results.push_back(test_backpressure_patterns());
    results.push_back(test_simultaneous_updates());
    results.push_back(test_binary_replay_reproducibility());

    if (!out_path.empty()) {
        std::ofstream ofs(out_path);
        ofs << "{\n  \"schema_version\": \"fpga-ob.v1\",\n  \"scenarios\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            ofs << "    {"
                << "\"name\":\"" << r.name << "\","
                << "\"passed\":" << (r.passed ? "true" : "false") << ","
                << "\"events\":" << r.events << ","
                << "\"mismatches\":" << r.mismatches << ","
                << "\"cycles\":" << r.cycles << ","
                << "\"backpressure_stalls\":" << r.stalls << ","
                << "\"dual_lane_cycles\":" << r.dual_lane_cycles
                << "}" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        ofs << "  ]\n}\n";
    }

    if (g_failures == 0) {
        std::cout << "[PASS] All integration scenarios passed (" << results.size() << " scenarios, 0 failures).\n";
        return 0;
    }
    std::cerr << "[FAIL] Integration tests finished with " << g_failures << " failures.\n";
    return 1;
}
