// Verilator C++ Differential Testbench Harness for rtl/order_book_top.sv
// Built via `make sim-verilator` when Verilator is installed.

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "Vorder_book_top.h"
#include "verilated.h"
#include "fpga_ob/contracts.hpp"
#include "fpga_ob/reference_book.hpp"

namespace {

// Pack EventPacket into 160-bit VlWide<5> matching order_book_pkg::event_pkt_t
// [159:128] seq_no, [127:96] timestamp_ns, [95:64] price, [63:32] qty,
// [31:24] opcode, [23:16] side, [15:8] flags, [7:0] schema_ver
void pack_event(VlWide<5>& dst, const fpga_ob::EventPacket& e) {
    dst[0] = (static_cast<uint32_t>(e.opcode) << 24) |
             (static_cast<uint32_t>(e.side) << 16) |
             (static_cast<uint32_t>(e.flags) << 8) |
             static_cast<uint32_t>(e.schema_ver);
    dst[1] = e.qty;
    dst[2] = e.price;
    dst[3] = e.timestamp_ns;
    dst[4] = e.seq_no;
}

// Helper to extract bit slice [hi:lo] from VlWide<N>
template <size_t N>
uint32_t get_bits(const VlWide<N>& w, int lo, int width) {
    uint32_t val = 0;
    for (int i = 0; i < width; ++i) {
        int bit = lo + i;
        uint32_t b = (w[bit / 32] >> (bit % 32)) & 1u;
        val |= (b << i);
    }
    return val;
}

template <size_t N>
fpga_ob::BboOutputPacket unpack_bbo(const VlWide<N>& w) {
    // Matches order_book_pkg::bbo_pkt_t MSB-to-LSB layout (total 355 bits):
    // active_ask_levels [15:0]
    // active_bid_levels [31:16]
    // crossed           [32]
    // spread            [64:33]
    // best_ask_orders   [80:65]
    // best_ask_qty      [112:81]
    // best_ask_price    [144:113]
    // best_ask_valid    [145]
    // best_bid_orders   [161:146]
    // best_bid_qty      [193:162]
    // best_bid_price    [225:194]
    // best_bid_valid    [226]
    // timestamp_ns      [258:227]
    // seq_no_1          [290:259]
    // seq_no_0          [322:291]
    // status_1          [330:323]
    // status_0          [338:331]
    // lane1_valid       [339]
    // schema_ver        [347:340]
    fpga_ob::BboOutputPacket o{};
    o.active_ask_levels = static_cast<uint16_t>(get_bits(w, 0, 16));
    o.active_bid_levels = static_cast<uint16_t>(get_bits(w, 16, 16));
    o.crossed           = get_bits(w, 32, 1) != 0;
    o.spread            = static_cast<int32_t>(get_bits(w, 33, 32));
    o.best_ask_orders   = static_cast<uint16_t>(get_bits(w, 65, 16));
    o.best_ask_qty      = get_bits(w, 81, 32);
    o.best_ask_price    = get_bits(w, 113, 32);
    o.best_ask_valid    = get_bits(w, 145, 1) != 0;
    o.best_bid_orders   = static_cast<uint16_t>(get_bits(w, 146, 16));
    o.best_bid_qty      = get_bits(w, 162, 32);
    o.best_bid_price    = get_bits(w, 194, 32);
    o.best_bid_valid    = get_bits(w, 226, 1) != 0;
    o.timestamp_ns      = get_bits(w, 227, 32);
    o.seq_no_1          = get_bits(w, 259, 32);
    o.seq_no_0          = get_bits(w, 291, 32);
    o.status_1          = static_cast<fpga_ob::Status>(get_bits(w, 323, 8));
    o.status_0          = static_cast<fpga_ob::Status>(get_bits(w, 331, 8));
    o.lane1_valid       = get_bits(w, 339, 1) != 0;
    o.schema_ver        = static_cast<uint8_t>(get_bits(w, 340, 8));
    return o;
}

void step_clock(Vorder_book_top& dut) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    size_t num_events = 10000;
    uint32_t seed = 20261127;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--events" && i + 1 < argc) num_events = std::stoull(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<uint32_t>(std::stoul(argv[++i]));
    }

    Vorder_book_top dut;
    fpga_ob::Config cfg{};
    fpga_ob::ReferenceOrderBook ref(cfg);

    // Synchronous reset
    dut.rst_n      = 0;
    dut.in_valid_0 = 0;
    dut.in_valid_1 = 0;
    dut.out_ready  = 1;
    step_clock(dut);
    step_clock(dut);
    dut.rst_n = 1;

    std::mt19937 rng(seed);
    std::vector<fpga_ob::BboOutputPacket> expected;
    std::vector<fpga_ob::BboOutputPacket> observed;
    expected.reserve(num_events);
    observed.reserve(num_events);

    uint32_t ts = 1000;
    for (size_t i = 0; i < num_events; ++i) {
        fpga_ob::EventPacket ev{};
        ev.seq_no       = static_cast<uint32_t>(i + 1);
        ev.timestamp_ns = ts;
        ts += 4;
        ev.side       = static_cast<uint8_t>(rng() & 1u);
        ev.price      = cfg.price_base + static_cast<uint32_t>(rng() % 24u);
        ev.qty        = 1u + static_cast<uint32_t>(rng() % 500u);
        uint32_t roll = rng() % 10u;
        if (roll < 6u) ev.opcode = static_cast<uint8_t>(fpga_ob::Opcode::Add);
        else if (roll < 8u) ev.opcode = static_cast<uint8_t>(fpga_ob::Opcode::Cancel);
        else ev.opcode = static_cast<uint8_t>(fpga_ob::Opcode::Execute);
        ev.schema_ver = fpga_ob::kSchemaVersion;

        expected.push_back(ref.apply_single(ev));

        dut.in_valid_0 = 1;
        dut.in_valid_1 = 0;
        pack_event(dut.in_event_0, ev);
        dut.out_ready = 1;
        step_clock(dut);
        if (dut.out_valid) {
            observed.push_back(unpack_bbo(dut.out_bbo));
        }
    }

    dut.in_valid_0 = 0;
    for (int i = 0; i < 6; ++i) {
        step_clock(dut);
        if (dut.out_valid) {
            observed.push_back(unpack_bbo(dut.out_bbo));
        }
    }

    size_t mismatches = 0;
    if (expected.size() != observed.size()) {
        std::cerr << "Count mismatch: expected=" << expected.size() << " observed=" << observed.size() << "\n";
        return 1;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!(expected[i] == observed[i])) {
            ++mismatches;
            if (mismatches <= 5) {
                std::cerr << "Mismatch at index " << i << "\n"
                          << "  expected: " << fpga_ob::to_json(expected[i]) << "\n"
                          << "  observed: " << fpga_ob::to_json(observed[i]) << "\n";
            }
        }
    }

    std::cout << "{\"simulator\":\"verilator\",\"events\":" << num_events
              << ",\"seed\":" << seed << ",\"mismatches\":" << mismatches << "}\n";
    return (mismatches == 0) ? 0 : 1;
}
