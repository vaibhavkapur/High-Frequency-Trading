#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

TEST(OrderBook, L3AddBuildsSortedLevels) {
    OrderBook b;
    b.apply(add(1, 1, Side::Buy, 100, 10));
    b.apply(add(2, 2, Side::Buy, 102, 5));
    b.apply(add(3, 3, Side::Buy, 101, 7));
    b.apply(add(4, 4, Side::Sell, 105, 3));
    b.apply(add(5, 5, Side::Sell, 104, 4));
    b.apply(add(6, 6, Side::Buy, 102, 1));

    ASSERT_NE(b.best_bid(), nullptr);
    EXPECT_EQ(b.best_bid()->price, 102);
    EXPECT_EQ(b.best_bid()->qty, 6);
    EXPECT_EQ(b.best_ask()->price, 104);
    EXPECT_EQ(b.side(Side::Buy).size(), 3u);
    EXPECT_EQ(b.side(Side::Buy).from_best(1).price, 101);
    EXPECT_EQ(b.side(Side::Buy).from_best(2).price, 100);
    EXPECT_EQ(b.side(Side::Sell).from_best(1).price, 105);
    EXPECT_DOUBLE_EQ(*b.mid(), 103.0);
    EXPECT_EQ(b.anomalies(), 0u);
}

TEST(OrderBook, L3CancelDeleteExecute) {
    OrderBook b;
    b.apply(add(1, 1, Side::Buy, 100, 10));
    b.apply(add(2, 2, Side::Buy, 100, 5));

    ApplyResult r = b.apply(cancel(3, 1, 4));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.side, Side::Buy);
    EXPECT_EQ(r.price, 100);
    EXPECT_EQ(r.qty, 4);
    EXPECT_EQ(r.before, 15);
    EXPECT_EQ(r.after, 11);
    EXPECT_EQ(r.seq, 1u);

    r = b.apply(exec(4, 2, 100)); // over-sized execution clamps to what rests
    EXPECT_EQ(r.qty, 5);
    EXPECT_EQ(b.best_bid()->qty, 6);
    EXPECT_EQ(b.l3_orders(), 1u);

    r = b.apply(del(5, 1));
    EXPECT_EQ(r.qty, 6);
    EXPECT_EQ(b.best_bid(), nullptr);
    EXPECT_FALSE(b.mid().has_value());
}

TEST(OrderBook, InconsistentEventsAreCountedAndIgnored) {
    OrderBook b;
    b.apply(add(1, 1, Side::Buy, 100, 10));
    EXPECT_FALSE(b.apply(add(2, 1, Side::Buy, 101, 10)).ok); // duplicate id
    EXPECT_FALSE(b.apply(cancel(3, 99, 1)).ok);              // unknown id
    EXPECT_FALSE(b.apply(exec(4, 99, 1)).ok);
    EXPECT_FALSE(b.apply(del(5, 99)).ok);
    EXPECT_FALSE(b.apply(add(6, 7, Side::Buy, 100, 0)).ok); // zero qty
    EXPECT_FALSE(b.apply(add(7, 8, Side::Buy, 0, 5)).ok);   // zero price
    EXPECT_FALSE(b.apply(cancel(8, 1, 0)).ok);
    EXPECT_FALSE(b.apply(level(9, Side::Buy, 100, -1)).ok);
    EXPECT_FALSE(b.apply(trade(10, Side::Buy, 100, 0)).ok);
    EXPECT_EQ(b.anomalies(), 9u);
    EXPECT_EQ(b.best_bid()->price, 100);
    EXPECT_EQ(b.best_bid()->qty, 10);
}

TEST(OrderBook, L2LevelSetAndRemove) {
    OrderBook b;
    b.apply(level(1, Side::Sell, 101, 8));
    b.apply(level(2, Side::Sell, 103, 2));
    b.apply(level(3, Side::Sell, 102, 5));
    EXPECT_EQ(b.best_ask()->price, 101);

    const ApplyResult r = b.apply(level(4, Side::Sell, 101, 3));
    EXPECT_EQ(r.before, 8);
    EXPECT_EQ(r.after, 3);
    EXPECT_EQ(r.qty, 5);

    b.apply(level(5, Side::Sell, 101, 0));
    EXPECT_EQ(b.best_ask()->price, 102);
    EXPECT_TRUE(b.apply(level(6, Side::Sell, 999, 0)).ok); // removing nothing is fine
    EXPECT_EQ(b.side(Side::Sell).size(), 2u);
}

TEST(OrderBook, L2TradeIsHeldPendingUntilLevelUpdate) {
    OrderBook b;
    b.apply(level(1, Side::Buy, 100, 20));
    ApplyResult r = b.apply(trade(2, Side::Sell, 100, 6)); // seller hits the bid
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.side, Side::Buy);
    EXPECT_EQ(b.best_bid()->qty, 20); // unchanged until the depth update
    EXPECT_EQ(b.best_bid()->pending_trade, 6);

    r = b.apply(level(3, Side::Buy, 100, 12));
    EXPECT_EQ(r.pending_trade, 6); // 6 of the 8 removed were the trade
    EXPECT_EQ(b.best_bid()->pending_trade, 0);
}
