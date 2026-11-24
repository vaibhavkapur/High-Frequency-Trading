#include <gtest/gtest.h>

#include "sor/audit_log.hpp"
#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/oms.hpp"
#include "sor/risk_manager.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

using namespace sor;

namespace {

struct OmsHarness {
    SystemConfig cfg = make_default_config();
    ConsolidatedBook book;
    RoutePlanner planner;
    RiskManager risk;
    VenueHealthMonitor health;
    AuditLog audit;
    OrderManagementSystem oms;

    OmsHarness() : planner(cfg.router), risk(cfg.risk), health(cfg.health),
                   oms(book, planner, risk, health, audit) {
        book.configure_venues(cfg.venues);
    }

    void seed_fresh_books(TimestampNs now) {
        book.set_level(0, Side::Buy, usd_to_ticks(149.99), 500, now - 100'000, now - 50'000);
        book.set_level(0, Side::Sell, usd_to_ticks(150.00), 500, now - 100'000, now - 50'000);
        book.set_level(1, Side::Buy, usd_to_ticks(149.99), 500, now - 100'000, now - 50'000);
        book.set_level(1, Side::Sell, usd_to_ticks(150.00), 500, now - 100'000, now - 50'000);
        book.set_level(2, Side::Buy, usd_to_ticks(149.99), 500, now - 100'000, now - 50'000);
        book.set_level(2, Side::Sell, usd_to_ticks(150.00), 500, now - 100'000, now - 50'000);
    }
};

}  // namespace

TEST(OmsTest, PartialFillsReduceRemainingParentQtyExactlyOnce) {
    OmsHarness h;
    const TimestampNs t0 = 5'000'000'000;
    h.seed_fresh_books(t0);

    std::vector<ChildCommand> cmds;
    ParentOrderRequest req{};
    req.client_order_id = "P-PARTIAL-1";
    req.side = Side::Buy;
    req.quantity = 300;
    req.limit_price = usd_to_ticks(150.05);
    req.tif = TimeInForce::DAY;
    req.timestamp_ns = t0;

    const OrderId pid = h.oms.submit_parent(req, cmds);
    ASSERT_EQ(cmds.size(), 1U);
    const OrderId cid = cmds[0].child_id;
    const VenueId vid = cmds[0].venue_id;

    std::vector<ChildCommand> follow;
    // First partial fill: 120 shares (exec_id = 500).
    ExecutionReport f1{};
    f1.type = ExecReportType::Fill;
    f1.child_id = cid;
    f1.venue_id = vid;
    f1.exec_id = 500;
    f1.fill_qty = 120;
    f1.fill_price = usd_to_ticks(150.00);
    f1.fee_usd = h.cfg.venues[vid].taker_fee_usd(f1.fill_price, 120);
    f1.timestamp_ns = t0 + 200'000;
    h.oms.on_execution_report(f1, follow);

    // Duplicate delivery of the EXACT same exec_id = 500 MUST be ignored!
    h.oms.on_execution_report(f1, follow);

    const auto* p = h.oms.get_parent(pid);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->filled_qty, 120);
    EXPECT_EQ(p->leaves_qty, 180);
    EXPECT_EQ(p->in_flight_qty, 180);
    EXPECT_EQ(p->state, ParentOrderState::PartiallyFilled);
    EXPECT_EQ(h.oms.stats().duplicate_execs_ignored, 1U);
    EXPECT_EQ(h.risk.net_position(), 120);

    // Second fill completes remaining 180 shares (exec_id = 501).
    ExecutionReport f2 = f1;
    f2.exec_id = 501;
    f2.fill_qty = 180;
    f2.fee_usd = h.cfg.venues[vid].taker_fee_usd(f2.fill_price, 180);
    f2.timestamp_ns = t0 + 300'000;
    h.oms.on_execution_report(f2, follow);

    EXPECT_EQ(p->filled_qty, 300);
    EXPECT_EQ(p->leaves_qty, 0);
    EXPECT_EQ(p->in_flight_qty, 0);
    EXPECT_EQ(p->state, ParentOrderState::Filled);
    EXPECT_EQ(h.risk.net_position(), 300);
    EXPECT_EQ(h.risk.open_buy_qty(), 0);
}

TEST(OmsTest, FailingVenueCannotReceiveNewlyRoutedChildOrdersAndFailsOver) {
    OmsHarness h;
    const TimestampNs t0 = 5'000'000'000;
    h.seed_fresh_books(t0);

    // Configure circuit breaker to trip after 2 consecutive rejects.
    VenueHealthConfig hcfg = h.health.config();
    hcfg.circuit_open_reject_threshold = 2;
    h.health.configure(hcfg);

    // Submit first parent -> routes to BATS_INV (venue 1, cheapest effective ask via rebate).
    std::vector<ChildCommand> cmds;
    ParentOrderRequest req1{};
    req1.client_order_id = "P-FAIL-1";
    req1.side = Side::Buy;
    req1.quantity = 200;
    req1.limit_price = usd_to_ticks(150.05);
    req1.tif = TimeInForce::IOC;
    req1.timestamp_ns = t0;

    const OrderId pid1 = h.oms.submit_parent(req1, cmds);
    ASSERT_EQ(cmds.size(), 1U);
    const VenueId failing_vid = cmds[0].venue_id;
    EXPECT_EQ(failing_vid, 1);

    // Venue 1 rejects -> automatic failover reroutes remaining 200 to a healthy venue (not 1!).
    std::vector<ChildCommand> failover_cmds;
    ExecutionReport rej1{};
    rej1.type = ExecReportType::Reject;
    rej1.child_id = cmds[0].child_id;
    rej1.venue_id = failing_vid;
    rej1.timestamp_ns = t0 + 100'000;
    rej1.reason = "gateway_error";
    h.oms.on_execution_report(rej1, failover_cmds);

    ASSERT_EQ(failover_cmds.size(), 1U);
    EXPECT_NE(failover_cmds[0].venue_id, failing_vid);
    EXPECT_EQ(failover_cmds[0].quantity, 200);
    EXPECT_EQ(h.oms.stats().failovers_triggered, 1U);

    // Trip circuit breaker completely on Venue 1 with a second reject.
    EXPECT_TRUE(h.health.record_reject(failing_vid, t0 + 150'000));
    EXPECT_FALSE(h.health.is_routable(failing_vid, t0 + 200'000));

    // New parent order MUST NOT route to Venue 1 while CircuitOpen!
    std::vector<ChildCommand> cmds2;
    ParentOrderRequest req2{};
    req2.client_order_id = "P-FAIL-2";
    req2.side = Side::Buy;
    req2.quantity = 200;
    req2.limit_price = usd_to_ticks(150.05);
    req2.tif = TimeInForce::IOC;
    req2.timestamp_ns = t0 + 200'000;
    (void)h.oms.submit_parent(req2, cmds2);

    ASSERT_FALSE(cmds2.empty());
    for (const auto& c : cmds2) {
        EXPECT_NE(c.venue_id, failing_vid);
    }

    // Complete first parent via failover child fill.
    ExecutionReport fill{};
    fill.type = ExecReportType::Fill;
    fill.child_id = failover_cmds[0].child_id;
    fill.venue_id = failover_cmds[0].venue_id;
    fill.exec_id = 777;
    fill.fill_qty = 200;
    fill.fill_price = usd_to_ticks(150.00);
    fill.timestamp_ns = t0 + 250'000;
    std::vector<ChildCommand> empty;
    h.oms.on_execution_report(fill, empty);
    EXPECT_EQ(h.oms.get_parent(pid1)->state, ParentOrderState::Filled);
}

TEST(OmsTest, CancelReplaceAndVenueOutageHandling) {
    OmsHarness h;
    const TimestampNs t0 = 5'000'000'000;
    h.seed_fresh_books(t0);

    std::vector<ChildCommand> cmds;
    ParentOrderRequest req{};
    req.client_order_id = "P-CR-1";
    req.side = Side::Buy;
    req.quantity = 200;
    req.limit_price = usd_to_ticks(150.05);
    req.tif = TimeInForce::DAY;
    req.timestamp_ns = t0;

    const OrderId pid = h.oms.submit_parent(req, cmds);
    ASSERT_EQ(cmds.size(), 1U);
    const OrderId cid = cmds[0].child_id;
    const VenueId vid = cmds[0].venue_id;

    // Ack then Replace child down to 150 @ $150.02.
    std::vector<ChildCommand> out;
    ExecutionReport ack{ExecReportType::Ack, cid, vid, 0, 0, 0, 0.0, t0 + 100'000, ""};
    h.oms.on_execution_report(ack, out);
    EXPECT_EQ(h.oms.get_child(cid)->state, ChildOrderState::Acked);

    ASSERT_TRUE(h.oms.replace_child(cid, 150, usd_to_ticks(150.02), t0 + 120'000, out));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, ChildCommandType::ReplaceOrder);

    ExecutionReport rep_ack{ExecReportType::ReplaceAck, cid, vid, 0, 0, 0, 0.0, t0 + 150'000, ""};
    out.clear();
    h.oms.on_execution_report(rep_ack, out);
    EXPECT_EQ(h.oms.get_child(cid)->orig_qty, 150);
    EXPECT_EQ(h.oms.get_child(cid)->limit_price, usd_to_ticks(150.02));

    // Trigger explicit venue outage on `vid` -> cancels active child and fails over!
    out.clear();
    h.oms.on_venue_down(vid, t0 + 200'000, out);
    EXPECT_FALSE(h.health.is_routable(vid, t0 + 250'000));
    bool saw_cancel = false;
    bool saw_failover_new = false;
    for (const auto& c : out) {
        if (c.type == ChildCommandType::CancelOrder && c.child_id == cid) saw_cancel = true;
        if (c.type == ChildCommandType::NewOrder && c.venue_id != vid) saw_failover_new = true;
    }
    EXPECT_TRUE(saw_cancel);
    EXPECT_TRUE(saw_failover_new);

    // Cancel entire parent -> cancels failover child and releases risk once acked.
    out.clear();
    ASSERT_TRUE(h.oms.cancel_parent(pid, t0 + 300'000, out));
    ASSERT_FALSE(out.empty());
    for (const auto& c : out) {
        ExecutionReport cxl_ack{ExecReportType::CancelAck, c.child_id, c.venue_id, 0, 0, 0, 0.0,
                                t0 + 350'000, ""};
        std::vector<ChildCommand> tmp;
        h.oms.on_execution_report(cxl_ack, tmp);
    }
    EXPECT_EQ(h.oms.get_parent(pid)->state, ParentOrderState::Cancelled);
    EXPECT_EQ(h.risk.open_buy_qty(), 0);
}

TEST(RiskManagerTest, EnforcesLimitsDuplicatePreventionAndPriceCollar) {
    RiskLimits lim{};
    lim.max_order_qty = 1000;
    lim.max_order_notional_usd = 200'000.0;
    lim.max_net_position_qty = 1500;
    lim.max_gross_position_qty = 2000;
    lim.max_open_notional_usd = 250'000.0;
    lim.max_orders_per_second = 3;
    lim.max_price_collar_bps = 100.0;  // 1% collar

    RiskManager rm(lim);
    const double mid = static_cast<double>(usd_to_ticks(100.00));
    const TimestampNs t0 = 1'000'000'000;

    // 1) Valid order approved.
    auto r1 = rm.check_and_reserve({"CID-1", Side::Buy, 800, usd_to_ticks(100.50), mid, t0});
    EXPECT_TRUE(r1.approved);
    EXPECT_EQ(rm.open_buy_qty(), 800);

    // 2) Duplicate client_order_id rejected.
    auto rdup = rm.check_and_reserve({"CID-1", Side::Buy, 100, usd_to_ticks(100.10), mid, t0 + 100});
    EXPECT_FALSE(rdup.approved);
    EXPECT_EQ(rdup.reason, RiskRejectReason::DuplicateClientOrderId);

    // 3) Exceeds max_order_qty.
    auto rqty = rm.check_and_reserve({"CID-2", Side::Buy, 1200, usd_to_ticks(100.00), mid, t0 + 200});
    EXPECT_FALSE(rqty.approved);
    EXPECT_EQ(rqty.reason, RiskRejectReason::MaxOrderQtyExceeded);

    // 4) Price collar breached (> 100 bps from $100.00 mid).
    auto rcol = rm.check_and_reserve({"CID-3", Side::Buy, 100, usd_to_ticks(105.00), mid, t0 + 300});
    EXPECT_FALSE(rcol.approved);
    EXPECT_EQ(rcol.reason, RiskRejectReason::PriceCollarBreached);

    // 5) Projected net position breached (800 open + 800 new = 1600 > 1500).
    auto rpos = rm.check_and_reserve({"CID-4", Side::Buy, 800, usd_to_ticks(100.00), mid, t0 + 400});
    EXPECT_FALSE(rpos.approved);
    EXPECT_EQ(rpos.reason, RiskRejectReason::MaxNetPositionExceeded);

    // 6) Rate limit: 2 more orders pass (total 3 in 1s), 4th fails until 1s passes.
    EXPECT_TRUE(rm.check_and_reserve({"CID-5", Side::Buy, 100, usd_to_ticks(100.00), mid, t0 + 500}).approved);
    EXPECT_TRUE(rm.check_and_reserve({"CID-6", Side::Buy, 100, usd_to_ticks(100.00), mid, t0 + 600}).approved);
    auto rrate = rm.check_and_reserve({"CID-7", Side::Buy, 100, usd_to_ticks(100.00), mid, t0 + 700});
    EXPECT_FALSE(rrate.approved);
    EXPECT_EQ(rrate.reason, RiskRejectReason::OrderRateLimitExceeded);

    // After 1s slides past t0, rate limit allows new order.
    EXPECT_TRUE(
        rm.check_and_reserve({"CID-8", Side::Buy, 100, usd_to_ticks(100.00), mid, t0 + 1'000'000'001})
            .approved);

    // 7) Kill switch blocks everything.
    rm.set_kill_switch(true);
    auto rks = rm.check_and_reserve({"CID-9", Side::Buy, 10, usd_to_ticks(100.00), mid, t0 + 2'000'000'000});
    EXPECT_FALSE(rks.approved);
    EXPECT_EQ(rks.reason, RiskRejectReason::KillSwitchActive);
}
