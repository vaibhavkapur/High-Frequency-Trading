#include "mmae/feature_engine.hpp"
#include "mmae/io_adapter.hpp"
#include "mmae/order_book.hpp"
#include "mmae/types.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    }
}

void check_near(double actual, double expected, double tol, const std::string& msg) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tol) {
        std::cerr << "[FAIL] " << msg << " (actual=" << actual << ", expected=" << expected
                  << ", tol=" << tol << ")\n";
        ++g_failures;
    }
}

std::string serialize_rows(const std::vector<mmae::FeatureRow>& rows) {
    std::ostringstream oss;
    mmae::write_features_csv_header(oss);
    for (const auto& r : rows) {
        mmae::write_feature_csv_row(oss, r);
    }
    return oss.str();
}

void test_hand_worked_fixture(const std::string& fixtures_dir, const std::string& variant) {
    std::vector<mmae::ParsedRow> parsed;
    std::string io_err;
    bool loaded = mmae::load_events_csv(fixtures_dir + "/hand_worked_l3.csv", parsed, io_err);
    check(loaded, variant + ": load hand_worked_l3.csv (" + io_err + ")");
    if (!loaded) return;

    mmae::EngineConfig cfg{};
    cfg.variant = variant;
    cfg.tick_size = 0.01;
    cfg.stale_threshold_ns = 1'000'000'000LL;
    cfg.flow_window_ns = 1'000'000'000LL;
    cfg.depth_levels_k = 2;

    mmae::FeatureEngine engine(cfg);
    std::vector<mmae::FeatureRow> rows;
    for (const auto& pr : parsed) {
        check(pr.ok, variant + ": hand_worked parse ok at line " + std::to_string(pr.line_number));
        auto r = engine.process_event(pr.event, pr.line_number);
        check(r.accepted, variant + ": hand_worked accepted seq=" + std::to_string(pr.event.seq_num));
        rows.push_back(r.feature);
    }

    check(rows.size() == 12, variant + ": expected 12 feature rows");
    if (rows.size() != 12) return;

    // seq 1: one-sided book
    check(!rows[0].book_valid, variant + ": seq 1 book invalid");
    check(rows[0].bid_qty_l1 == 100 && rows[0].ask_qty_l1 == 0, variant + ": seq 1 L1 qtys");

    // seq 2: two-sided valid book
    check(rows[1].book_valid, variant + ": seq 2 book valid");
    check(rows[1].spread_ticks == 2, variant + ": seq 2 spread_ticks == 2");
    check_near(rows[1].mid_price, 100.01, 1e-9, variant + ": seq 2 mid_price");
    check_near(rows[1].microprice, 100.005, 1e-9, variant + ": seq 2 microprice");
    check_near(rows[1].microprice_offset_ticks, -0.5, 1e-9, variant + ": seq 2 microprice offset");
    check_near(rows[1].obi_l1, -0.5, 1e-9, variant + ": seq 2 obi_l1");
    check_near(rows[1].ofi_event, 0.0, 1e-9, variant + ": seq 2 ofi_event");

    // seq 3: add 200 to bid L1 -> OFI +200
    check(rows[2].bid_l1_order_count == 2, variant + ": seq 3 bid_l1_order_count == 2");
    check_near(rows[2].obi_l1, 0.0, 1e-9, variant + ": seq 3 obi_l1 == 0");
    check_near(rows[2].ofi_event, 200.0, 1e-9, variant + ": seq 3 ofi_event == 200");

    // seq 4: L2 bid depth -> obi_k == +0.4
    check(rows[3].bid_depth_k == 700 && rows[3].ask_depth_k == 300, variant + ": seq 4 depth_k");
    check_near(rows[3].obi_k, 0.4, 1e-9, variant + ": seq 4 obi_k == 0.4");

    // seq 5: BUY at ask
    check(rows[4].trade_sign == mmae::TradeSign::BUY, variant + ": seq 5 BUY");
    check(rows[4].classification_reason == mmae::ClassificationReason::QUOTE_AT_OR_ABOVE_ASK, variant + ": seq 5 reason");
    check(!rows[4].is_ambiguous, variant + ": seq 5 not ambiguous");
    check(rows[4].signed_qty == 50, variant + ": seq 5 signed_qty == 50");

    // seq 6: exact midpoint -> explicit AMBIGUOUS
    check(rows[5].trade_sign == mmae::TradeSign::AMBIGUOUS, variant + ": seq 6 AMBIGUOUS");
    check(rows[5].classification_reason == mmae::ClassificationReason::MIDPOINT_AMBIGUOUS, variant + ": seq 6 midpoint reason");
    check(rows[5].is_ambiguous, variant + ": seq 6 is_ambiguous");
    check(rows[5].tick_rule_hint == -1, variant + ": seq 6 tick_rule_hint == -1");
    check(rows[5].signed_qty == 0, variant + ": seq 6 signed_qty == 0");
    check(rows[5].rolling_ambiguous_qty == 40, variant + ": seq 6 rolling_ambiguous_qty == 40");

    // seq 7: SELL at bid
    check(rows[6].trade_sign == mmae::TradeSign::SELL, variant + ": seq 7 SELL");
    check(rows[6].signed_qty == -30, variant + ": seq 7 signed_qty == -30");
    check_near(rows[6].rolling_trade_flow_imbalance, 0.25, 1e-9, variant + ": seq 7 TFI == 0.25");

    // seq 8: FIFO head execution
    check(rows[7].exec_queue_ahead_qty == 0, variant + ": seq 8 ahead_qty == 0");
    check(rows[7].exec_queue_ahead_orders == 0, variant + ": seq 8 ahead_orders == 0");
    check(!rows[7].fifo_anomaly, variant + ": seq 8 no fifo anomaly");
    check(rows[7].top_bid_depletion_qty == 60, variant + ": seq 8 bid depletion == 60");
    check_near(rows[7].ofi_event, -60.0, 1e-9, variant + ": seq 8 ofi == -60");

    // seq 9: non-FIFO execution behind order 101 (40 qty remaining ahead)
    check(rows[8].exec_queue_ahead_qty == 40, variant + ": seq 9 ahead_qty == 40");
    check(rows[8].exec_queue_ahead_orders == 1, variant + ": seq 9 ahead_orders == 1");
    check_near(rows[8].exec_queue_position_ratio, 40.0 / 240.0, 1e-9, variant + ": seq 9 queue ratio");
    check(rows[8].fifo_anomaly, variant + ": seq 9 fifo anomaly flagged");

    // seq 10: partial ask cancel
    check(rows[9].top_ask_depletion_qty == 100, variant + ": seq 10 ask depletion == 100");
    check_near(rows[9].ofi_event, 100.0, 1e-9, variant + ": seq 10 ofi == +100");
    check_near(rows[9].cancel_to_add_ratio, 0.25, 1e-9, variant + ": seq 10 cancel/add == 0.25");

    // seq 11: bid price improvement
    check(rows[10].best_bid_ticks == 10001 && rows[10].spread_ticks == 1, variant + ": seq 11 bid improved");
    check_near(rows[10].ofi_event, 150.0, 1e-9, variant + ": seq 11 ofi == +150");
    check_near(rows[10].ofi_window, 340.0, 1e-9, variant + ": seq 11 ofi_window == 340");

    // seq 12: full ask cancel -> one-sided book
    check(!rows[11].book_valid && !rows[11].has_ask, variant + ": seq 12 ask empty");
    check(rows[11].top_ask_depletion_qty == 200, variant + ": seq 12 ask depletion == 200");
    check_near(rows[11].cancel_to_add_ratio, 0.5, 1e-9, variant + ": seq 12 cancel/add == 0.5");

    const auto& d = engine.diagnostics();
    check(d.total_trades == 3 && d.classified_buys == 1 && d.classified_sells == 1 && d.ambiguous_trades == 1,
          variant + ": trade classification counts");
    check(d.fifo_anomalies == 1, variant + ": fifo_anomalies == 1");
}

void test_crossed_locked_stale_and_gaps(const std::string& fixtures_dir) {
    std::vector<mmae::ParsedRow> parsed;
    std::string io_err;
    bool loaded = mmae::load_events_csv(fixtures_dir + "/edge_cases_crossed_stale_gaps.csv", parsed, io_err);
    check(loaded, "load edge_cases_crossed_stale_gaps.csv");
    if (!loaded) return;

    mmae::EngineConfig cfg{};
    cfg.variant = "optimized";
    cfg.stale_threshold_ns = 1'000'000'000LL;  // 1s
    mmae::FeatureEngine engine(cfg);

    std::vector<mmae::FeatureRow> rows;
    for (const auto& pr : parsed) {
        auto r = engine.process_event(pr.event, pr.line_number);
        check(r.accepted, "edge_cases accepted seq=" + std::to_string(pr.event.seq_num));
        rows.push_back(r.feature);
    }

    // seq 3: locked book (bid=10002, ask=10002); seq 4: trade during locked book -> BOOK_LOCKED
    check(rows[2].is_locked && !rows[2].book_valid, "seq 3 locked book flagged");
    check(rows[3].is_ambiguous && rows[3].classification_reason == mmae::ClassificationReason::BOOK_LOCKED,
          "seq 4 trade ambiguous due to BOOK_LOCKED");

    // seq 5: crossed book (bid=10003 > ask=10002); seq 6: trade -> BOOK_CROSSED
    check(rows[4].is_crossed && !rows[4].book_valid, "seq 5 crossed book flagged");
    check(rows[5].is_ambiguous && rows[5].classification_reason == mmae::ClassificationReason::BOOK_CROSSED,
          "seq 6 trade ambiguous due to BOOK_CROSSED");

    // seq 9: heartbeat after 1.93s -> stale; seq 10: trade -> BOOK_STALE
    check(rows[8].is_stale && !rows[8].book_valid, "seq 9 heartbeat stale book flagged");
    check(rows[9].is_ambiguous && rows[9].classification_reason == mmae::ClassificationReason::BOOK_STALE,
          "seq 10 trade ambiguous due to BOOK_STALE");

    // seq 15: gap (expected 11, observed 15, missing 4); seq 16: trade -> GAP_ACTIVE
    check(rows[10].gap_detected_here && rows[10].gap_active && !rows[10].book_valid, "seq 15 gap flagged");
    check(rows[11].is_ambiguous && rows[11].classification_reason == mmae::ClassificationReason::GAP_ACTIVE,
          "seq 16 trade ambiguous due to GAP_ACTIVE");

    // seq 17: L2_SNAPSHOT_CLEAR resolves gap; seq 20: trade valid BUY
    check(!rows[12].gap_active, "seq 17 snapshot cleared gap_active");
    check(!rows[15].is_ambiguous && rows[15].trade_sign == mmae::TradeSign::BUY,
          "seq 20 trade classified BUY after snapshot recovery");

    // seq 25: second unresolved gap (expected 21, observed 25, missing 4)
    check(rows[16].gap_detected_here && rows[16].gap_active, "seq 25 second gap flagged");

    const auto& d = engine.diagnostics();
    check(d.gap_count == 2 && d.gaps.size() == 2, "all 2 gap periods recorded");
    if (d.gaps.size() == 2) {
        check(d.gaps[0].expected_seq == 11 && d.gaps[0].observed_seq == 15 && d.gaps[0].resolved_by_snapshot,
              "gap 0 metadata & snapshot resolution");
        check(d.gaps[1].expected_seq == 21 && d.gaps[1].observed_seq == 25 && !d.gaps[1].resolved_by_snapshot,
              "gap 1 metadata & unresolved status");
    }
}

void test_no_future_quote_leakage() {
    // Prefix events 1..5 followed by two drastically different futures A and B.
    std::vector<mmae::Event> prefix = {
        {1000000000LL, 1000000100LL, 1, "CAUSAL", mmae::EventType::L3_ADD, mmae::Side::BID, 10000, 100, 1},
        {1010000000LL, 1010000100LL, 2, "CAUSAL", mmae::EventType::L3_ADD, mmae::Side::ASK, 10002, 200, 2},
        {1020000000LL, 1020000100LL, 3, "CAUSAL", mmae::EventType::L3_ADD, mmae::Side::BID, 10000, 150, 3},
        {1030000000LL, 1030000100LL, 4, "CAUSAL", mmae::EventType::TRADE, mmae::Side::UNKNOWN, 10002, 50, 0},
        {1040000000LL, 1040000100LL, 5, "CAUSAL", mmae::EventType::TRADE, mmae::Side::UNKNOWN, 10001, 25, 0},
    };

    std::vector<mmae::Event> stream_a = prefix;
    stream_a.push_back({1050000000LL, 1050000100LL, 6, "CAUSAL", mmae::EventType::L3_ADD, mmae::Side::BID, 10500, 9999, 4});
    stream_a.push_back({1060000000LL, 1060000100LL, 7, "CAUSAL", mmae::EventType::TRADE, mmae::Side::UNKNOWN, 10500, 500, 0});

    std::vector<mmae::Event> stream_b = prefix;
    stream_b.push_back({1050000000LL, 1050000100LL, 6, "CAUSAL", mmae::EventType::L3_CANCEL, mmae::Side::BID, 0, 0, 1});
    stream_b.push_back({1060000000LL, 1060000100LL, 7, "CAUSAL", mmae::EventType::L3_ADD, mmae::Side::ASK, 9500, 8888, 5});

    mmae::EngineConfig cfg{};
    cfg.variant = "optimized";
    mmae::FeatureEngine eng_a(cfg);
    mmae::FeatureEngine eng_b(cfg);

    std::vector<mmae::FeatureRow> pref_a, pref_b;
    for (size_t i = 0; i < stream_a.size(); ++i) {
        auto ra = eng_a.process_event(stream_a[i], i + 2);
        auto rb = eng_b.process_event(stream_b[i], i + 2);
        if (i < prefix.size()) {
            pref_a.push_back(ra.feature);
            pref_b.push_back(rb.feature);
        }
    }

    check(serialize_rows(pref_a) == serialize_rows(pref_b),
          "no future quote used in feature calculation (exact prefix invariance)");
}

void test_capacity_and_validation_errors(const std::string& fixtures_dir) {
    // 1. Bounded capacity test on both variants.
    for (const std::string& variant : {"baseline", "optimized"}) {
        mmae::EngineConfig cfg{};
        cfg.variant = variant;
        cfg.max_levels = 2;
        cfg.max_orders = 2;
        cfg.depth_levels_k = 2;
        mmae::FeatureEngine eng(cfg);

        auto r1 = eng.process_event({1000, 1000, 1, "CAP", mmae::EventType::L3_ADD, mmae::Side::BID, 100, 10, 1});
        auto r2 = eng.process_event({2000, 2000, 2, "CAP", mmae::EventType::L3_ADD, mmae::Side::BID, 99, 10, 2});
        // 3rd order exceeds max_orders=2
        auto r3 = eng.process_event({3000, 3000, 3, "CAP", mmae::EventType::L3_ADD, mmae::Side::BID, 99, 10, 3});
        check(r1.accepted && r2.accepted, variant + ": first 2 orders accepted");
        check(!r3.accepted && r3.error == mmae::ValidationErrorCode::BOOK_CAPACITY_ORDERS_EXCEEDED,
              variant + ": 3rd order rejected with BOOK_CAPACITY_ORDERS_EXCEEDED");

        // Cancel order 2 and test max_levels=2 via L2_SET
        auto r4 = eng.process_event({4000, 4000, 4, "CAP", mmae::EventType::L2_SET, mmae::Side::BID, 98, 10, 0});
        check(!r4.accepted && r4.error == mmae::ValidationErrorCode::BOOK_CAPACITY_LEVELS_EXCEEDED,
              variant + ": 3rd price level rejected with BOOK_CAPACITY_LEVELS_EXCEEDED");
        check(eng.diagnostics().capacity_errors == 2, variant + ": capacity_errors == 2");
    }

    // 2. Invalid events CSV fixture.
    std::vector<mmae::ParsedRow> parsed;
    std::string io_err;
    bool loaded = mmae::load_events_csv(fixtures_dir + "/invalid_events.csv", parsed, io_err);
    check(loaded, "load invalid_events.csv");
    if (!loaded) return;

    mmae::EngineConfig cfg{};
    mmae::FeatureEngine eng(cfg);
    uint64_t parse_rejects = 0;
    for (const auto& pr : parsed) {
        if (!pr.ok) {
            ++parse_rejects;
            continue;
        }
        (void)eng.process_event(pr.event, pr.line_number);
    }
    // Out of 11 rows: 2 valid (seq 1 and seq 8), 9 invalid (2 parse errors + 7 domain validation errors).
    check(eng.diagnostics().accepted_events == 2, "invalid_events.csv accepted_events == 2");
    check(parse_rejects == 2, "invalid_events.csv parse_rejects == 2");
    check(eng.diagnostics().rejected_events == 7, "invalid_events.csv domain rejected_events == 7");
    check(eng.diagnostics().out_of_order_events == 1, "invalid_events.csv out_of_order_events == 1");
    check(eng.diagnostics().duplicate_seq_events == 1, "invalid_events.csv duplicate_seq_events == 1");
    check(eng.diagnostics().unknown_order_errors == 1, "invalid_events.csv unknown_order_errors == 1");
}

void test_l3_fifo_priority_and_parity() {
    // Verify modify down preserves FIFO priority, modify up loses FIFO priority,
    // and baseline vs optimized produce byte-identical feature output.
    std::vector<mmae::Event> events = {
        {1000, 1000, 1, "FIFO", mmae::EventType::L3_ADD, mmae::Side::BID, 1000, 100, 10},
        {2000, 2000, 2, "FIFO", mmae::EventType::L3_ADD, mmae::Side::BID, 1000, 100, 20},
        {3000, 3000, 3, "FIFO", mmae::EventType::L3_ADD, mmae::Side::ASK, 1002, 100, 30},
        // Reduce order 10 qty from 100 -> 60: must STAY ahead of order 20.
        {4000, 4000, 4, "FIFO", mmae::EventType::L3_MODIFY, mmae::Side::BID, 1000, 60, 10},
        // Execute order 20: should still see 60 ahead (order 10).
        {5000, 5000, 5, "FIFO", mmae::EventType::L3_EXECUTE, mmae::Side::BID, 1000, 20, 20},
        // Increase order 10 qty from 60 -> 120: loses priority, moves BEHIND order 20 (qty 80).
        {6000, 6000, 6, "FIFO", mmae::EventType::L3_MODIFY, mmae::Side::BID, 1000, 120, 10},
        // Execute order 10: should now see 80 ahead (order 20)!
        {7000, 7000, 7, "FIFO", mmae::EventType::L3_EXECUTE, mmae::Side::BID, 1000, 30, 10},
    };

    std::vector<mmae::FeatureRow> base_rows, opt_rows;
    for (const std::string& variant : {"baseline", "optimized"}) {
        mmae::EngineConfig cfg{};
        cfg.variant = variant;
        mmae::FeatureEngine eng(cfg);
        auto& out = (variant == "baseline") ? base_rows : opt_rows;
        for (const auto& ev : events) {
            auto r = eng.process_event(ev);
            check(r.accepted, variant + ": FIFO event accepted seq=" + std::to_string(ev.seq_num));
            out.push_back(r.feature);
        }
        check(out[4].exec_queue_ahead_qty == 60, variant + ": modify-down preserved priority (60 ahead)");
        check(out[6].exec_queue_ahead_qty == 80, variant + ": modify-up lost priority (80 ahead)");
    }
    check(serialize_rows(base_rows) == serialize_rows(opt_rows),
          "baseline vs optimized byte-identical parity on FIFO suite");
}

}  // namespace

int main(int argc, char** argv) {
    std::string fixtures_dir = "fixtures";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--fixtures-dir" && i + 1 < argc) {
            fixtures_dir = argv[++i];
        }
    }

    test_hand_worked_fixture(fixtures_dir, "baseline");
    test_hand_worked_fixture(fixtures_dir, "optimized");
    test_crossed_locked_stale_and_gaps(fixtures_dir);
    test_no_future_quote_leakage();
    test_capacity_and_validation_errors(fixtures_dir);
    test_l3_fifo_priority_and_parity();

    if (g_failures > 0) {
        std::cerr << "FAILED: " << g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cout << "ALL C++ TESTS PASSED.\n";
    return 0;
}
