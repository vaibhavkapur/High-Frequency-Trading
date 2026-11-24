#include <gtest/gtest.h>

#include "hftbt/strategies.hpp"
#include "hftbt/synth.hpp"
#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

TEST(MarketMaker, QuotesBothSidesAroundMidAndRequotesWhenItMoves) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 10),
        add(1000, 2, Side::Sell, 110, 10), // mid 105
        add(2000, 3, Side::Buy, 104, 10),  // mid 107
        add(9000, 4, Side::Sell, 200, 1),
    });
    MarketMakerParams p;
    p.half_spread_ticks = 2;
    p.order_qty = 3;
    p.skew_ticks_per_lot = 0.0;
    MarketMaker mm(p);
    EngineConfig cfg;
    cfg.end_ts = 1500;
    {
        Engine engine(feed, cfg, mm);
        engine.run();
        const SimOrder* bid = engine.exchange().find(1);
        const SimOrder* ask = engine.exchange().find(2);
        ASSERT_NE(bid, nullptr);
        ASSERT_NE(ask, nullptr);
        EXPECT_EQ(bid->price, 103);
        EXPECT_EQ(ask->price, 107);
        EXPECT_EQ(bid->qty, 3);
    }
    cfg.end_ts = 0;
    MarketMaker mm2(p);
    Engine engine(feed, cfg, mm2);
    engine.run();
    EXPECT_EQ(engine.summary().cancels_sent, 2u);
    EXPECT_EQ(engine.summary().orders_sent, 4u);
    EXPECT_EQ(engine.exchange().open_orders(), 2u);
    EXPECT_EQ(engine.exchange().find(3)->price + engine.exchange().find(4)->price, 105 + 109);
}

TEST(MarketMaker, StaysInsideInventoryLimitAndSkewsAgainstPosition) {
    const FeedData feed = generate_l3({.seed = 31, .events = 80000});
    MarketMakerParams p;
    p.half_spread_ticks = 1;
    p.order_qty = 10;
    p.max_position = 25;
    p.skew_ticks_per_lot = 0.1;
    MarketMaker mm(p);
    EngineConfig cfg;
    cfg.latency.md_delay_ns = 20'000;
    cfg.latency.order_delay_ns = 50'000;
    cfg.latency.response_delay_ns = 50'000;
    cfg.sample_interval_ns = 1'000'000;
    Engine engine(feed, cfg, mm);
    Qty max_abs = 0;
    engine.start();
    while (engine.step())
        max_abs = std::max<Qty>(max_abs, std::abs(engine.portfolio().account(0).position));
    engine.finish();
    ASSERT_GT(engine.summary().fills, 20u);
    EXPECT_GT(max_abs, 0);
    // Quote sizes are capped by remaining room, so even with every quote
    // filling during the latency window the TRUE position stays bounded.
    EXPECT_LE(max_abs, p.max_position);
    EXPECT_EQ(engine.summary().taker_fills, 0u); // post-only: never crosses
}

TEST(ImbalanceTaker, BuysWhenBidHeavySellsWhenAskHeavy) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 90),
        add(1000, 2, Side::Sell, 101, 10), // imbalance +0.8
        add(5'000'000, 3, Side::Sell, 101, 500),
        del(5'000'001, 1),
        add(5'000'002, 4, Side::Buy, 100, 10), // imbalance about -0.96
        add(9'000'000, 5, Side::Sell, 300, 1),
    });
    ImbalanceParams p;
    p.threshold = 0.6;
    p.order_qty = 8;
    p.max_position = 4;
    ImbalanceTaker strat(p);
    Engine engine(feed, {}, strat);
    engine.run();
    const auto& fills = engine.fills();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].side, Side::Buy);
    EXPECT_EQ(fills[0].price, 101);
    EXPECT_EQ(fills[0].qty, 4); // capped by max_position
    EXPECT_FALSE(fills[0].maker);
    EXPECT_EQ(fills[1].side, Side::Sell);
    EXPECT_EQ(fills[1].price, 100);
    EXPECT_EQ(fills[1].qty, 8); // from +4 straight to the -4 limit
    EXPECT_EQ(engine.portfolio().account(0).position, -4);
    EXPECT_DOUBLE_EQ(engine.portfolio().account(0).realized, -4.0); // paid the spread
}

TEST(Ladder, RestsRequestedNumberOfOrders) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 10),
        add(1000, 2, Side::Sell, 102, 10),
        add(9000, 3, Side::Sell, 300, 1),
    });
    Ladder strat(LadderParams{.levels = 6, .order_qty = 2, .offset_ticks = 3});
    Engine engine(feed, {}, strat);
    engine.run();
    EXPECT_EQ(engine.exchange().open_orders(), 12u);
    EXPECT_EQ(engine.summary().fills, 0u);
}
