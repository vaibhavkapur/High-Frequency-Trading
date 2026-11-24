#include <gtest/gtest.h>

#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

using namespace sor;

TEST(RoutePlannerTest, StaleBestAskDoesNotWinRouting) {
    auto cfg = make_default_config();
    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;

    const TimestampNs now = 20'000'000'000;
    // Venue 0 (ARCA): fresh ask at $150.0100.
    book.set_level(0, Side::Buy, usd_to_ticks(149.99), 500, now - 200'000, now - 100'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.01), 500, now - 200'000, now - 100'000);

    // Venue 1 (BATS_INV): STALE ask at $149.9500 (way cheaper, but 8ms old > 5ms timeout!).
    book.set_level(1, Side::Buy, usd_to_ticks(149.94), 500, now - 8'500'000, now - 8'000'000);
    book.set_level(1, Side::Sell, usd_to_ticks(149.95), 500, now - 8'500'000, now - 8'000'000);

    RouterConfig rcfg{};
    rcfg.policy = RoutingPolicy::SmartCostAware;
    rcfg.respect_staleness = true;
    RoutePlanner planner(rcfg);

    RouteRequest req{};
    req.side = Side::Buy;
    req.qty = 200;
    req.limit_price = usd_to_ticks(150.05);
    req.tif = TimeInForce::IOC;
    req.now_ns = now;

    const auto plan = planner.plan(book, &health, req);
    ASSERT_EQ(plan.slice_count, 1U);
    EXPECT_EQ(plan.slices[0].venue_id, 0);  // Must route to fresh ARCA, NOT stale BATS_INV!
    EXPECT_EQ(plan.slices[0].quantity, 200);

    // Verify BATS_INV is explicitly annotated as STALE_QUOTE in venue evaluations.
    bool saw_stale = false;
    for (std::size_t i = 0; i < plan.eval_count; ++i) {
        if (plan.venue_evals[i].venue_id == 1) {
            EXPECT_EQ(plan.venue_evals[i].reason, IneligibilityReason::StaleQuote);
            saw_stale = true;
        }
    }
    EXPECT_TRUE(saw_stale);
}

TEST(RoutePlannerTest, HigherExchangeFeesReverseNaivePriceOnlyChoice) {
    auto cfg = make_default_config();
    // Equalize latency & fill probability so fee inversion is isolated cleanly.
    for (auto& v : cfg.venues) {
        v.base_one_way_latency_ns = 100'000;
        v.base_fill_prob = 0.95;
        v.latency_decay_per_ms = 0.10;
        v.depletion_sensitivity = 0.10;
    }
    cfg.venues[0].taker_fee_per_share_ticks = 35;   // +$0.0035/share high fee
    cfg.venues[1].taker_fee_per_share_ticks = -25;  // -$0.0025/share rebate

    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;

    const TimestampNs now = 5'000'000'000;
    // Venue 0 has $0.0020 better raw ask ($150.0000 vs $150.0020),
    // BUT effective ask: V0 = $150.0035 vs V1 = $149.9995 -> V1 is $0.0040/share cheaper net!
    book.set_level(0, Side::Buy, usd_to_ticks(149.9900), 500, now - 100'000, now - 50'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.0000), 500, now - 100'000, now - 50'000);
    book.set_level(1, Side::Buy, usd_to_ticks(149.9880), 500, now - 100'000, now - 50'000);
    book.set_level(1, Side::Sell, usd_to_ticks(150.0020), 500, now - 100'000, now - 50'000);

    RouteRequest req{};
    req.side = Side::Buy;
    req.qty = 200;
    req.limit_price = usd_to_ticks(150.05);
    req.tif = TimeInForce::IOC;
    req.now_ns = now;

    // Naive best-price picks Venue 0.
    RouterConfig naive_cfg{};
    naive_cfg.policy = RoutingPolicy::NaiveBestPrice;
    const auto naive_plan = RoutePlanner(naive_cfg).plan(book, &health, req);
    ASSERT_EQ(naive_plan.slice_count, 1U);
    EXPECT_EQ(naive_plan.slices[0].venue_id, 0);

    // Fee-aware & SmartCostAware reverse the choice and pick Venue 1!
    RouterConfig fee_cfg{};
    fee_cfg.policy = RoutingPolicy::FeeAware;
    const auto fee_plan = RoutePlanner(fee_cfg).plan(book, &health, req);
    ASSERT_EQ(fee_plan.slice_count, 1U);
    EXPECT_EQ(fee_plan.slices[0].venue_id, 1);

    RouterConfig smart_cfg{};
    smart_cfg.policy = RoutingPolicy::SmartCostAware;
    const auto smart_plan = RoutePlanner(smart_cfg).plan(book, &health, req);
    ASSERT_EQ(smart_plan.slice_count, 1U);
    EXPECT_EQ(smart_plan.slices[0].venue_id, 1);

    // Check explicit cost breakdown sums cleanly.
    const auto& c = smart_plan.slices[0].cost;
    EXPECT_NEAR(c.raw_price_cost_bps + c.explicit_fee_bps + c.latency_risk_bps + c.miss_penalty_bps,
                c.total_expected_cost_bps, 1e-9);
    EXPECT_LT(c.explicit_fee_bps, 0.0);  // Rebate is negative cost!
}

TEST(RoutePlannerTest, RespectsParentQtyLotSizeAndVenueChildLimits) {
    auto cfg = make_default_config();
    cfg.venues[0].max_child_qty = 100;
    cfg.venues[0].lot_size = 50;
    cfg.venues[0].min_order_qty = 50;
    cfg.venues[0].max_active_children = 2;  // Max 2 children * 100 = 200 shares on Venue 0.

    cfg.venues[1].max_child_qty = 150;
    cfg.venues[1].lot_size = 25;
    cfg.venues[1].min_order_qty = 50;
    cfg.venues[1].max_parent_share = 0.5;  // Max 50% of parent order on Venue 1.

    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;

    const TimestampNs now = 1'000'000'000;
    book.set_level(0, Side::Sell, usd_to_ticks(100.00), 1000, now - 10'000, now - 5'000);
    book.set_level(1, Side::Sell, usd_to_ticks(100.00), 1000, now - 10'000, now - 5'000);
    book.set_level(2, Side::Sell, usd_to_ticks(100.01), 1000, now - 10'000, now - 5'000);

    RoutePlanner planner;
    RouteRequest req{};
    req.side = Side::Buy;
    req.qty = 400;
    req.limit_price = usd_to_ticks(100.05);
    req.tif = TimeInForce::IOC;
    req.now_ns = now;

    const auto plan = planner.plan(book, &health, req);
    EXPECT_EQ(plan.planned_qty, 400);
    EXPECT_EQ(plan.unroutable_qty, 0);

    Quantity v0_qty = 0;
    Quantity v1_qty = 0;
    std::uint32_t v0_children = 0;
    for (std::size_t i = 0; i < plan.slice_count; ++i) {
        const auto& s = plan.slices[i];
        const auto& vcfg = cfg.venues[s.venue_id];
        EXPECT_GE(s.quantity, vcfg.min_order_qty);
        EXPECT_LE(s.quantity, vcfg.max_child_qty);
        EXPECT_EQ(s.quantity % vcfg.lot_size, 0);
        if (s.venue_id == 0) {
            v0_qty += s.quantity;
            ++v0_children;
        } else if (s.venue_id == 1) {
            v1_qty += s.quantity;
        }
    }
    EXPECT_LE(v0_children, 2U);
    EXPECT_LE(v0_qty, 200);
    EXPECT_LE(v1_qty, 200);  // 50% of 400
}

TEST(RoutePlannerTest, FOKFailsWhenInsufficientEligibleLiquidity) {
    auto cfg = make_default_config();
    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;

    const TimestampNs now = 1'000'000'000;
    book.set_level(0, Side::Sell, usd_to_ticks(100.00), 150, now - 10'000, now - 5'000);

    RouteRequest req{};
    req.side = Side::Buy;
    req.qty = 300;
    req.limit_price = usd_to_ticks(100.05);
    req.tif = TimeInForce::FOK;
    req.now_ns = now;

    const auto plan = RoutePlanner().plan(book, &health, req);
    EXPECT_TRUE(plan.fok_failed);
    EXPECT_EQ(plan.slice_count, 0U);
    EXPECT_EQ(plan.planned_qty, 0);
}

TEST(RoutePlannerTest, LatencyAndFillProbabilityShiftAwayFromSlowVenue) {
    auto cfg = make_default_config();
    // Same fees and same raw price, stark latency difference.
    for (auto& v : cfg.venues) {
        v.taker_fee_per_share_ticks = 10;
    }
    cfg.venues[0].base_one_way_latency_ns = 50'000;     // 50 us fast
    cfg.venues[0].base_fill_prob = 0.96;
    cfg.venues[1].base_one_way_latency_ns = 2'000'000;  // 2 ms slow
    cfg.venues[1].base_fill_prob = 0.65;

    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;

    const TimestampNs now = 2'000'000'000;
    book.set_level(0, Side::Sell, usd_to_ticks(100.00), 300, now - 20'000, now - 10'000);
    book.set_level(1, Side::Sell, usd_to_ticks(100.00), 300, now - 20'000, now - 10'000);

    RouteRequest req{};
    req.side = Side::Buy;
    req.qty = 200;
    req.limit_price = usd_to_ticks(100.05);
    req.tif = TimeInForce::IOC;
    req.now_ns = now;

    const auto plan = RoutePlanner().plan(book, &health, req);
    ASSERT_EQ(plan.slice_count, 1U);
    EXPECT_EQ(plan.slices[0].venue_id, 0);  // Fast high-fill-prob venue wins!
}
