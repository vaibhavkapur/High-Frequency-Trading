// Fill-model tests against the exchange simulator in isolation.
#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

namespace {
struct Fixture {
    explicit Fixture(ExchangeConfig cfg = {}) : ex(1, cfg, rec) {}
    Recorder rec;
    Exchange ex;
    void feed(std::initializer_list<MarketEvent> evs) {
        for (const MarketEvent& e : evs) ex.on_market_event(e);
    }
};
} // namespace

// ------------------------------------------------------------------ L3 queue

TEST(ExchangeL3, OrderWaitsBehindQueueThenFillsPartially) {
    Fixture f;
    f.feed({add(1, 1, Side::Buy, 100, 10), add(2, 2, Side::Buy, 100, 5),
            add(3, 9, Side::Sell, 102, 50)});
    f.ex.on_new_order(limit(1000, Side::Buy, 100, 4), 10);
    ASSERT_NE(f.ex.find(1000), nullptr);
    EXPECT_EQ(f.ex.find(1000)->ahead, 15);

    f.feed({exec(11, 1, 10)}); // ahead of us
    EXPECT_EQ(f.ex.find(1000)->ahead, 5);
    f.feed({add(12, 3, Side::Buy, 100, 7)}); // joins behind us
    EXPECT_EQ(f.ex.find(1000)->ahead, 5);
    f.feed({exec(13, 2, 5)}); // last order ahead of us
    EXPECT_EQ(f.ex.find(1000)->ahead, 0);
    EXPECT_TRUE(f.rec.fills().empty());

    // The order behind us trades: we must have traded first.
    f.feed({exec(14, 3, 3)});
    ASSERT_EQ(f.rec.fills().size(), 1u);
    EXPECT_EQ(f.rec.fills()[0].qty, 3);
    EXPECT_EQ(f.rec.fills()[0].leaves, 1);
    EXPECT_EQ(f.rec.fills()[0].price, 100);
    EXPECT_TRUE(f.rec.fills()[0].maker);
    EXPECT_EQ(f.rec.fills()[0].exch_ts, 14);

    f.feed({exec(15, 3, 4)});
    EXPECT_EQ(f.rec.filled(1000), 4);
    EXPECT_EQ(f.ex.find(1000), nullptr);
    EXPECT_EQ(f.ex.open_orders(), 0u);
    f.feed({exec(16, 3, 1)}); // nothing left to over-fill
    EXPECT_EQ(f.rec.filled(1000), 4);
}

TEST(ExchangeL3, CancelsAheadAdvanceUsCancelsBehindDoNot) {
    Fixture f;
    f.feed({add(1, 1, Side::Sell, 200, 10), add(2, 2, Side::Sell, 200, 6)});
    f.ex.on_new_order(limit(1000, Side::Sell, 200, 5), 10);
    f.feed({add(11, 3, Side::Sell, 200, 8)});
    EXPECT_EQ(f.ex.find(1000)->ahead, 16);
    f.feed({cancel(12, 1, 4)});
    EXPECT_EQ(f.ex.find(1000)->ahead, 12);
    f.feed({del(13, 3)}); // behind us
    EXPECT_EQ(f.ex.find(1000)->ahead, 12);
    f.feed({del(14, 2)});
    EXPECT_EQ(f.ex.find(1000)->ahead, 6);
    f.feed({add(15, 4, Side::Sell, 199, 3), del(16, 4)}); // other price level
    EXPECT_EQ(f.ex.find(1000)->ahead, 6);
    EXPECT_TRUE(f.rec.fills().empty());
}

TEST(ExchangeL3, TradeThroughFillsBetterPricedOrderFirst) {
    Fixture f;
    f.feed({add(1, 1, Side::Buy, 100, 10), add(2, 2, Side::Sell, 105, 10)});
    f.ex.on_new_order(limit(1000, Side::Buy, 102, 6), 10); // improves the bid
    EXPECT_EQ(f.ex.find(1000)->ahead, 0);
    f.feed({exec(11, 1, 4)}); // someone sold 4 at 100; we were bidding 102
    ASSERT_EQ(f.rec.fills().size(), 1u);
    EXPECT_EQ(f.rec.fills()[0].qty, 4);     // capped by the historical trade size
    EXPECT_EQ(f.rec.fills()[0].price, 102); // at OUR price, not the print
    f.feed({exec(12, 1, 6)});
    EXPECT_EQ(f.rec.filled(1000), 6);
}

TEST(ExchangeL3, OneHistoricalTradeIsSharedAcrossOurOrdersInPriority) {
    Fixture f;
    f.feed({add(1, 1, Side::Buy, 100, 5), add(2, 9, Side::Sell, 110, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 100, 3), 10);
    f.ex.on_new_order(limit(1001, Side::Buy, 101, 2), 11); // better price: first in line
    f.ex.on_new_order(limit(1002, Side::Buy, 100, 9), 12);
    f.feed({del(13, 1), add(14, 2, Side::Buy, 100, 20)});
    f.feed({exec(15, 2, 6)}); // 6 lots: 2 to the 101 bid, 3 to order 1000, 1 to 1002
    EXPECT_EQ(f.rec.filled(1001), 2);
    EXPECT_EQ(f.rec.filled(1000), 3);
    EXPECT_EQ(f.rec.filled(1002), 1);
    Qty total = 0;
    for (const ExecReport& r : f.rec.fills()) total += r.qty;
    EXPECT_EQ(total, 6); // never more than actually traded
}

TEST(ExchangeL3, CrossingQuoteFillsRestingOrderWhenEnabled) {
    for (const bool enabled : {true, false}) {
        ExchangeConfig cfg;
        cfg.fill_on_crossing_quote = enabled;
        Fixture f(cfg);
        f.feed({add(1, 1, Side::Buy, 100, 10), add(2, 2, Side::Sell, 105, 10)});
        f.ex.on_new_order(limit(1000, Side::Buy, 103, 8), 10);
        f.feed({add(11, 3, Side::Sell, 104, 5)}); // does not reach our bid
        EXPECT_TRUE(f.rec.fills().empty());
        f.feed({add(12, 4, Side::Sell, 102, 5)}); // would have hit our 103 bid
        EXPECT_EQ(f.rec.filled(1000), enabled ? 5 : 0);
        if (enabled) EXPECT_EQ(f.rec.fills()[0].price, 103);
    }
}

// ------------------------------------------------------------------ L2 queue

TEST(ExchangeL2, TradeConsumesQueueAheadThenOverflowFillsUs) {
    Fixture f;
    f.feed({level(1, Side::Buy, 100, 20), level(2, Side::Sell, 101, 20)});
    f.ex.on_new_order(limit(1000, Side::Buy, 100, 5), 10);
    EXPECT_EQ(f.ex.find(1000)->ahead, 20);

    f.feed({trade(11, Side::Sell, 100, 8)});
    EXPECT_EQ(f.ex.find(1000)->ahead, 12);
    f.feed({level(12, Side::Buy, 100, 12)}); // depth update explained by the trade
    EXPECT_EQ(f.ex.find(1000)->ahead, 12);   // not double counted
    EXPECT_TRUE(f.rec.fills().empty());

    f.feed({trade(13, Side::Sell, 100, 15)}); // 12 ahead + 3 for us
    ASSERT_EQ(f.rec.fills().size(), 1u);
    EXPECT_EQ(f.rec.fills()[0].qty, 3);
    EXPECT_EQ(f.ex.find(1000)->ahead, 0);
    f.feed({trade(14, Side::Sell, 100, 50)});
    EXPECT_EQ(f.rec.filled(1000), 5);
}

TEST(ExchangeL2, QueueModelsDifferOnlyOnUnexplainedCancels) {
    const auto ahead_after = [](QueueModel m) {
        ExchangeConfig cfg;
        cfg.queue_model = m;
        Fixture f(cfg);
        f.feed({level(1, Side::Buy, 100, 20)});
        f.ex.on_new_order(limit(1000, Side::Buy, 100, 5), 10);
        f.feed({level(11, Side::Buy, 100, 30)}); // 10 lots join behind us
        f.feed({level(12, Side::Buy, 100, 24)}); // 6 lots cancel: from where?
        return f.ex.find(1000)->ahead;
    };
    EXPECT_EQ(ahead_after(QueueModel::Conservative), 20); // all 6 from behind
    EXPECT_EQ(ahead_after(QueueModel::Proportional), 16); // 6 * 20/30 from ahead
    EXPECT_EQ(ahead_after(QueueModel::Optimistic), 14);   // all 6 from ahead
}

TEST(ExchangeL2, AheadNeverExceedsDisplayedQtyEvenWhenConservative) {
    Fixture f; // conservative
    f.feed({level(1, Side::Sell, 50, 40)});
    f.ex.on_new_order(limit(1000, Side::Sell, 50, 5), 10);
    f.feed({level(11, Side::Sell, 50, 7)}); // 33 cancelled; at most 7 can be ahead
    EXPECT_EQ(f.ex.find(1000)->ahead, 7);
    f.feed({level(12, Side::Sell, 50, 0)});
    EXPECT_EQ(f.ex.find(1000)->ahead, 0);
    EXPECT_TRUE(f.rec.fills().empty()); // cancels alone never fill us
}

TEST(ExchangeL2, TradeThroughAndCrossingLevelIncrease) {
    Fixture f;
    f.feed({level(1, Side::Buy, 100, 10), level(2, Side::Sell, 105, 10)});
    f.ex.on_new_order(limit(1000, Side::Sell, 103, 4), 10);
    f.feed({trade(11, Side::Buy, 105, 3)}); // buyer paid 105 while we offered 103
    EXPECT_EQ(f.rec.filled(1000), 3);
    f.feed({level(12, Side::Buy, 103, 9)}); // a bid appears at our offer
    EXPECT_EQ(f.rec.filled(1000), 4);
    EXPECT_EQ(f.rec.fills()[1].price, 103);
}

// --------------------------------------------------------------------- taker

TEST(ExchangeTaker, WalksLevelsWithinLimitAndRestsRemainder) {
    Fixture f;
    f.feed({add(1, 1, Side::Sell, 101, 5), add(2, 2, Side::Sell, 102, 5),
            add(3, 3, Side::Sell, 103, 5), add(4, 4, Side::Buy, 99, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 102, 12), 10);
    const auto fills = f.rec.fills();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].price, 101);
    EXPECT_EQ(fills[0].qty, 5);
    EXPECT_FALSE(fills[0].maker);
    EXPECT_EQ(fills[1].price, 102);
    EXPECT_EQ(fills[1].qty, 5);
    EXPECT_EQ(fills[1].leaves, 2);
    EXPECT_EQ(f.rec.reports.front().type, ExecType::Ack);
    ASSERT_NE(f.ex.find(1000), nullptr); // 2 lots rest at 102
    EXPECT_EQ(f.ex.find(1000)->remaining, 2);
    EXPECT_EQ(f.ex.find(1000)->ahead, 0);
    // No market impact: the historical book is untouched.
    EXPECT_EQ(f.ex.book(0).best_ask()->price, 101);
}

TEST(ExchangeTaker, IocCancelsRemainderAndMarketSweeps) {
    Fixture f;
    f.feed({add(1, 1, Side::Buy, 100, 5), add(2, 2, Side::Buy, 99, 5)});
    f.ex.on_new_order(limit(1000, Side::Sell, 100, 8, Tif::IOC), 10);
    EXPECT_EQ(f.rec.filled(1000), 5);
    EXPECT_EQ(f.rec.last(1000)->type, ExecType::Cancelled);
    EXPECT_EQ(f.rec.last(1000)->qty, 3);
    EXPECT_EQ(f.ex.open_orders(), 0u);

    OrderRequest m = limit(1001, Side::Sell, 0, 7);
    m.type = OrdType::Market;
    f.feed({add(11, 3, Side::Buy, 100, 5)}); // 10 displayed at 100, 5 already taken by us
    f.ex.on_new_order(m, 12);
    EXPECT_EQ(f.rec.filled(1001), 7); // 5 @ 100 + 2 @ 99
    EXPECT_EQ(f.rec.last(1001)->price, 99);
}

TEST(ExchangeTaker, CannotTakeTheSameDisplayedLotsTwice) {
    Fixture f;
    f.feed({add(1, 1, Side::Sell, 101, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 101, 4, Tif::IOC), 10);
    f.ex.on_new_order(limit(1001, Side::Buy, 101, 4, Tif::IOC), 11);
    EXPECT_EQ(f.rec.filled(1000), 4);
    EXPECT_EQ(f.rec.filled(1001), 1); // only 1 of the 5 displayed lots was left
    f.ex.on_new_order(limit(1002, Side::Buy, 101, 4, Tif::IOC), 12);
    EXPECT_EQ(f.rec.filled(1002), 0);
    f.feed({add(13, 2, Side::Sell, 101, 2)}); // only the new lots are takeable
    f.ex.on_new_order(limit(1003, Side::Buy, 101, 10, Tif::IOC), 14);
    EXPECT_EQ(f.rec.filled(1003), 2);
    // History then trades the 5 lots we had taken: they are gone for everyone,
    // and a later add is fully available again.
    f.feed({exec(15, 1, 5), add(16, 3, Side::Sell, 101, 4)});
    f.ex.on_new_order(limit(1004, Side::Buy, 101, 10, Tif::IOC), 17);
    EXPECT_EQ(f.rec.filled(1004), 4);
}

TEST(ExchangeTaker, SlippagePenalisesTakerFillsOnly) {
    ExchangeConfig cfg;
    cfg.taker_slippage_ticks = 2;
    Fixture f(cfg);
    f.feed({add(1, 1, Side::Sell, 101, 5), add(2, 2, Side::Buy, 99, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 101, 5, Tif::IOC), 10);
    f.ex.on_new_order(limit(1001, Side::Sell, 99, 5, Tif::IOC), 11);
    EXPECT_EQ(f.rec.fills()[0].price, 103); // buy pays more
    EXPECT_EQ(f.rec.fills()[1].price, 97);  // sell receives less
}

TEST(ExchangeTaker, PostOnlyIsRejectedWhenItWouldCross) {
    Fixture f;
    f.feed({add(1, 1, Side::Sell, 101, 5), add(2, 2, Side::Buy, 99, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 101, 5, Tif::GTC, true), 10);
    ASSERT_EQ(f.rec.reports.size(), 1u);
    EXPECT_EQ(f.rec.reports[0].type, ExecType::Rejected);
    EXPECT_EQ(f.rec.reports[0].reason, RejectReason::PostOnlyWouldCross);
    f.ex.on_new_order(limit(1001, Side::Buy, 100, 5, Tif::GTC, true), 11);
    EXPECT_EQ(f.rec.last(1001)->type, ExecType::Ack);
    EXPECT_NE(f.ex.find(1001), nullptr);
}

// ------------------------------------------------------------ cancel / reject

TEST(ExchangeCancel, CancelRestingOrderAndCancelUnknownOrder) {
    Fixture f;
    f.feed({add(1, 1, Side::Buy, 100, 5)});
    f.ex.on_new_order(limit(1000, Side::Buy, 100, 5), 10);
    f.ex.on_cancel({1000, 0}, 20);
    EXPECT_EQ(f.rec.last(1000)->type, ExecType::Cancelled);
    EXPECT_EQ(f.rec.last(1000)->qty, 5);
    EXPECT_EQ(f.rec.last(1000)->side, Side::Buy);
    EXPECT_EQ(f.ex.open_orders(), 0u);

    f.ex.on_cancel({1000, 0}, 30); // already gone
    EXPECT_EQ(f.rec.last(1000)->type, ExecType::CancelRejected);
    EXPECT_EQ(f.rec.last(1000)->reason, RejectReason::TooLateToCancel);
    f.feed({add(31, 2, Side::Buy, 100, 5), exec(32, 2, 5)});
    EXPECT_EQ(f.rec.filled(1000), 0); // a cancelled order never fills
}

TEST(ExchangeReject, MalformedOrders) {
    Fixture f;
    OrderRequest bad_instr = limit(1, Side::Buy, 100, 5);
    bad_instr.instrument = 7;
    OrderRequest market_post_only = limit(4, Side::Buy, 0, 5);
    market_post_only.type = OrdType::Market;
    market_post_only.post_only = true;
    for (const OrderRequest& r :
         {bad_instr, limit(2, Side::Buy, 100, 0), limit(3, Side::Buy, 0, 5), market_post_only}) {
        f.ex.on_new_order(r, 10);
        EXPECT_EQ(f.rec.last(r.id)->type, ExecType::Rejected);
        EXPECT_EQ(f.rec.last(r.id)->reason, RejectReason::InvalidOrder);
    }
    f.ex.on_new_order(limit(5, Side::Buy, 100, 5), 10);
    f.ex.on_new_order(limit(5, Side::Buy, 100, 5), 11); // duplicate live id
    EXPECT_EQ(f.rec.last(5)->type, ExecType::Rejected);
    EXPECT_EQ(f.ex.open_orders(), 1u);
}
