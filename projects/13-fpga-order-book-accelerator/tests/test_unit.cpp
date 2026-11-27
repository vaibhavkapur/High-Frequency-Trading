#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "fpga_ob/contracts.hpp"
#include "fpga_ob/reference_book.hpp"
#include "fpga_ob/rtl_model.hpp"

using namespace fpga_ob;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    }
}

std::vector<BboOutputPacket> run_single_lane(PipelinedOrderBookAccelerator& dut,
                                             const std::vector<EventPacket>& events) {
    std::vector<BboOutputPacket> out;
    out.reserve(events.size());
    for (const auto& ev : events) {
        auto res = dut.tick(TickInput{
            .rst_n = true, .in_valid_0 = true, .in_event_0 = ev, .out_ready = true
        });
        check(res.accepted, "Single-lane event must be accepted when out_ready=1");
        if (res.out_valid) out.push_back(res.out_bbo);
    }
    for (uint32_t i = 0; i < PipelinedOrderBookAccelerator::kPipelineLatencyCycles + 1; ++i) {
        auto res = dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
        if (res.out_valid) out.push_back(res.out_bbo);
    }
    return out;
}

void test_wire_serialization() {
    EventPacket ev{};
    ev.seq_no       = 0x12345678u;
    ev.timestamp_ns = 0x9ABCDEF0u;
    ev.price        = 10042u;
    ev.qty          = 500000u;
    ev.opcode       = static_cast<uint8_t>(Opcode::Add);
    ev.side         = static_cast<uint8_t>(Side::Ask);
    ev.flags        = 0;
    ev.schema_ver   = kSchemaVersion;

    auto bytes   = encode_event(ev);
    auto decoded = decode_event(bytes);
    check(bytes.size() == kWireEventSize, "Wire event size must be 20 bytes");
    check(decoded == ev, "Wire encode/decode round-trip must be bit-exact");
}

void test_config_validation() {
    check(!Config{10000, 64, 32}.validate().has_value(), "Default config must be valid");
    check(Config{10000, 63, 32}.validate().has_value(), "Non-power-of-2 num_levels must fail");
    check(Config{10000, 1, 1}.validate().has_value(), "num_levels < 2 must fail");
    check(Config{10000, 64, 0}.validate().has_value(), "max_active_levels == 0 must fail");
    check(Config{10000, 64, 65}.validate().has_value(), "max_active_levels > num_levels must fail");
    check(Config{0xFFFFFFF0u, 64, 32}.validate().has_value(), "price_base + num_levels overflow must fail");
}

void test_deterministic_fixture() {
    auto bundle   = load_events_json("tests/fixtures/deterministic_basic.json");
    auto expected = load_expected_json("tests/fixtures/deterministic_basic.expected.json");

    PipelinedOrderBookAccelerator dut(bundle.config);
    ReferenceOrderBook ref(bundle.config);
    dut.tick(TickInput{.rst_n = false});

    auto got = run_single_lane(dut, bundle.events);
    check(got.size() == expected.size(), "Deterministic fixture output count mismatch");
    for (size_t i = 0; i < got.size() && i < expected.size(); ++i) {
        auto ref_out = ref.apply_single(bundle.events[i]);
        check(got[i] == expected[i], "DUT vs expected JSON mismatch at index " + std::to_string(i));
        check(got[i] == ref_out, "DUT vs ReferenceOrderBook mismatch at index " + std::to_string(i));
    }
    check(dut.diagnostics().events_committed == bundle.events.size(), "All deterministic events should commit");
    check(dut.diagnostics().events_rejected == 0, "No deterministic events should be rejected");
}

void test_invalid_and_boundary_fixture() {
    auto bundle = load_events_json("tests/fixtures/invalid_and_boundary.json");
    PipelinedOrderBookAccelerator dut(bundle.config);
    ReferenceOrderBook ref(bundle.config);
    dut.tick(TickInput{.rst_n = false});

    auto got = run_single_lane(dut, bundle.events);
    check(got.size() == bundle.events.size(), "Boundary fixture output count mismatch");

    const std::vector<Status> expected_statuses = {
        Status::ErrUnsupportedSchema,  // seq 1
        Status::ErrUnsupportedFlags,   // seq 2
        Status::ErrUnsupportedOpcode,  // seq 3
        Status::ErrInvalidSide,        // seq 4
        Status::ErrPriceOob,           // seq 5 (low)
        Status::ErrPriceOob,           // seq 6 (high)
        Status::ErrZeroQty,            // seq 7
        Status::ErrInvalidCancel,      // seq 8 (empty level)
        Status::ErrInvalidExecute,     // seq 9 (empty level)
        Status::Ok,                    // seq 10 (add near UINT32_MAX)
        Status::ErrOverflowQty,        // seq 11 (qty overflow)
        Status::Ok,                    // seq 12 (2nd active bid level -> at capacity 2)
        Status::ErrFullCapacity,       // seq 13 (3rd active bid level rejected)
        Status::ErrInvalidCancel,      // seq 14 (over-cancel 51 > 50)
        Status::ErrInvalidExecute,     // seq 15 (over-execute 51 > 50)
        Status::Ok,                    // seq 16 (cancel 50 -> frees slot)
        Status::Ok,                    // seq 17 (now 10005 fits cleanly)
    };

    for (size_t i = 0; i < got.size(); ++i) {
        auto ref_out = ref.apply_single(bundle.events[i]);
        check(got[i] == ref_out, "Invalid/boundary DUT vs Reference mismatch at index " + std::to_string(i));
        check(got[i].status_0 == expected_statuses[i],
              "Unexpected status at seq " + std::to_string(i + 1) + ": got " + status_name(got[i].status_0));
    }

    const auto& d = dut.diagnostics();
    check(d.err_unsupported_schema == 1, "err_unsupported_schema counter == 1");
    check(d.err_unsupported_flags == 1, "err_unsupported_flags counter == 1");
    check(d.err_unsupported_opcode == 1, "err_unsupported_opcode counter == 1");
    check(d.err_invalid_side == 1, "err_invalid_side counter == 1");
    check(d.err_price_oob == 2, "err_price_oob counter == 2");
    check(d.err_zero_qty == 1, "err_zero_qty counter == 1");
    check(d.err_invalid_cancel == 2, "err_invalid_cancel counter == 2");
    check(d.err_invalid_execute == 2, "err_invalid_execute counter == 2");
    check(d.err_overflow_qty == 1, "err_overflow_qty counter == 1");
    check(d.err_full_capacity == 1, "err_full_capacity counter == 1");
}

void test_reset_mid_stream() {
    Config cfg{10000, 64, 32};
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);

    dut.tick(TickInput{.rst_n = false});

    // Inject 4 events so bank has state AND Stages 1 & 2 have in-flight events
    for (uint32_t i = 0; i < 4; ++i) {
        EventPacket ev{i + 1, (i + 1) * 4, 10010 + i, 100, static_cast<uint8_t>(Opcode::Add),
                       static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion};
        dut.tick(TickInput{.rst_n = true, .in_valid_0 = true, .in_event_0 = ev, .out_ready = true});
    }

    // Assert synchronous reset mid-stream while pipeline is full
    auto r = dut.tick(TickInput{.rst_n = false, .in_valid_0 = true, .out_ready = true});
    check(!r.out_valid, "out_valid must deassert immediately on synchronous reset");
    check(dut.diagnostics().reset_cycles == 2, "reset_cycles must track mid-stream reset");
    check(dut.diagnostics().events_accepted == 0, "counters must clear on synchronous reset");

    // Verify no stale in-flight events leak out after reset
    for (int i = 0; i < 4; ++i) {
        auto idle = dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
        check(!idle.out_valid, "No stale pipeline outputs may appear after mid-stream reset");
    }

    // Verify clean post-reset operation matches freshly reset reference
    ref.reset();
    std::vector<EventPacket> post = {
        {101, 1000, 10020, 50, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Ask), 0, kSchemaVersion},
        {102, 1004, 10010, 10, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {103, 1008, 10019, 75, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
    };
    auto got = run_single_lane(dut, post);
    check(got.size() == post.size(), "Post-reset output count mismatch");
    for (size_t i = 0; i < post.size(); ++i) {
        auto exp = ref.apply_single(post[i]);
        check(got[i] == exp, "Post-reset output mismatch at index " + std::to_string(i));
    }
    check(got[1].status_0 == Status::ErrInvalidCancel,
          "Pre-reset bid at 10010 must be gone after reset (cancel must fail)");
}

void test_invalid_cancel_exhaustive() {
    Config cfg{10000, 64, 32};
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::vector<EventPacket> evs = {
        {1, 10, 10005, 10, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {2, 20, 10005, 50, static_cast<uint8_t>(Opcode::Add),    static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {3, 30, 10006, 10, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {4, 40, 10005, 51, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {5, 50, 10005, 20, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {6, 60, 10005, 31, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {7, 70, 10005, 30, static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
        {8, 80, 10005, 1,  static_cast<uint8_t>(Opcode::Cancel), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion},
    };
    auto got = run_single_lane(dut, evs);
    for (size_t i = 0; i < evs.size(); ++i) {
        check(got[i] == ref.apply_single(evs[i]), "Invalid cancel case mismatch at " + std::to_string(i));
    }
    check(dut.diagnostics().err_invalid_cancel == 5, "Expected exactly 5 ERR_INVALID_CANCEL events");
    check(!got.back().best_bid_valid, "Book should be empty after final full cancel");
}

void test_full_capacity_and_order_overflow() {
    Config cfg{10000, 64, 32};
    PipelinedOrderBookAccelerator dut(cfg);
    ReferenceOrderBook ref(cfg);
    dut.tick(TickInput{.rst_n = false});

    std::vector<EventPacket> evs;
    uint32_t seq = 1;
    // Fill all 32 active bid levels (prices 10000..10031)
    for (uint32_t i = 0; i < 32; ++i) {
        evs.push_back({seq++, seq * 4, 10000 + i, 10, static_cast<uint8_t>(Opcode::Add),
                       static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion});
    }
    // 33rd distinct bid level -> ERR_FULL_CAPACITY
    evs.push_back({seq++, seq * 4, 10040, 10, static_cast<uint8_t>(Opcode::Add),
                   static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion});
    // Adding to existing active bid level 10031 must SUCCEED
    evs.push_back({seq++, seq * 4, 10031, 25, static_cast<uint8_t>(Opcode::Add),
                   static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion});
    // Adding an ask level must SUCCEED (separate capacity per side)
    evs.push_back({seq++, seq * 4, 10050, 99, static_cast<uint8_t>(Opcode::Add),
                   static_cast<uint8_t>(Side::Ask), 0, kSchemaVersion});
    // Cancel bid level 10000 completely -> frees 1 bid capacity slot
    evs.push_back({seq++, seq * 4, 10000, 10, static_cast<uint8_t>(Opcode::Cancel),
                   static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion});
    // Now 10040 must SUCCEED and become the new best bid
    evs.push_back({seq++, seq * 4, 10040, 15, static_cast<uint8_t>(Opcode::Add),
                   static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion});

    auto got = run_single_lane(dut, evs);
    for (size_t i = 0; i < evs.size(); ++i) {
        check(got[i] == ref.apply_single(evs[i]), "Full capacity mismatch at index " + std::to_string(i));
    }
    check(got[32].status_0 == Status::ErrFullCapacity, "33rd active bid level must return ERR_FULL_CAPACITY");
    check(got[33].status_0 == Status::Ok, "Add to already active level at capacity must succeed");
    check(got[34].status_0 == Status::Ok, "Ask bank capacity must be independent of bid bank");
    check(got[36].status_0 == Status::Ok && got[36].best_bid_price == 10040,
          "After freeing a slot, 10040 must succeed and become best bid");
    check(dut.diagnostics().max_active_bid_levels == 32, "max_active_bid_levels must be 32");

    // Verify 16-bit order_count overflow protection
    dut.hard_reset();
    ref.reset();
    dut.tick(TickInput{.rst_n = false});
    EventPacket add1{1, 4, 10000, 1, static_cast<uint8_t>(Opcode::Add), static_cast<uint8_t>(Side::Bid), 0, kSchemaVersion};
    for (uint32_t i = 0; i < 65536; ++i) {
        add1.seq_no = i + 1;
        auto exp = ref.apply_single(add1);
        auto res = dut.tick(TickInput{.rst_n = true, .in_valid_0 = true, .in_event_0 = add1, .out_ready = true});
        if (i == 65535) {
            check(exp.status_0 == Status::ErrOverflowOrders, "65536th order at one level must overflow orders");
        }
        (void)res;
    }
    for (int i = 0; i < 4; ++i) {
        auto res = dut.tick(TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
        if (res.out_valid && res.out_bbo.seq_no_0 == 65536) {
            check(res.out_bbo.status_0 == Status::ErrOverflowOrders, "DUT 65536th order must return ERR_OVERFLOW_ORDERS");
            check(res.out_bbo.best_bid_orders == 65535, "Order count must remain saturated/unchanged at 65535");
        }
    }
    check(dut.diagnostics().err_overflow_orders == 1, "err_overflow_orders counter must equal 1");
}

}  // namespace

int main() {
    test_wire_serialization();
    test_config_validation();
    test_deterministic_fixture();
    test_invalid_and_boundary_fixture();
    test_reset_mid_stream();
    test_invalid_cancel_exhaustive();
    test_full_capacity_and_order_overflow();

    if (g_failures == 0) {
        std::cout << "[PASS] All unit tests passed (7 test suites, 0 failures).\n";
        return 0;
    }
    std::cerr << "[FAIL] Unit tests finished with " << g_failures << " failures.\n";
    return 1;
}
