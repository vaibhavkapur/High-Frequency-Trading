// The README's causality requirements, each as an executable statement.
#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

namespace {
EngineConfig latency(Timestamp md, Timestamp order, Timestamp response, Timestamp compute = 0) {
    EngineConfig c;
    c.latency.md_delay_ns = md;
    c.latency.order_delay_ns = order;
    c.latency.response_delay_ns = response;
    c.latency.compute_delay_ns = compute;
    return c;
}
} // namespace

TEST(Causality, StrategyCannotSeeAnEventBeforeItsObservableTime) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 10),
        add(1000, 2, Side::Sell, 102, 10),
        add(1300, 3, Side::Buy, 101, 5), // improves the bid
        del(1600, 2),                    // ask disappears
        add(5000, 4, Side::Sell, 103, 1),
    });
    Script s;
    Engine* eng = nullptr;
    std::vector<Timestamp> seen_at;
    s.md = [&](Context& c, const MarketEvent& e) {
        seen_at.push_back(c.now());
        EXPECT_EQ(c.now(), e.ts + 500) << "event observed at the wrong time";
        if (e.ts == 1000) {
            // At t=1500 the truth already has the 101 bid (t=1300); the
            // strategy must still see 100.
            EXPECT_EQ(c.now(), 1500);
            EXPECT_EQ(eng->exchange().book(0).best_bid()->price, 101);
            EXPECT_EQ(c.book(0).best_bid()->price, 100);
        }
        if (e.ts == 1300) {
            // At t=1800 the true ask is gone (t=1600) but still visible to us.
            EXPECT_EQ(eng->exchange().book(0).best_ask(), nullptr);
            ASSERT_NE(c.book(0).best_ask(), nullptr);
            EXPECT_EQ(c.book(0).best_ask()->price, 102);
            EXPECT_EQ(c.book(0).best_bid()->price, 101);
        }
    };
    Engine engine(feed, latency(500, 0, 0), s);
    eng = &engine;
    engine.run();
    // The t=5000 event would be observed at 5500, after the run ends at 5000.
    EXPECT_EQ(seen_at, (std::vector<Timestamp>{1500, 1500, 1800, 2100}));
}

TEST(Causality, NoFillBeforeTheOrderReachesTheExchange) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 50),
        add(1000, 2, Side::Sell, 105, 50),
        exec(1500, 1, 5), // our 102 bid is still in flight: must NOT fill
        exec(2500, 1, 5), // now it is resting and better priced: fills
        add(9000, 3, Side::Sell, 110, 1),
    });
    Script s;
    bool sent = false;
    s.md = [&](Context& c, const MarketEvent&) {
        if (sent) return;
        sent = true;
        EXPECT_EQ(c.now(), 1000);
        ASSERT_TRUE(c.send_limit(0, Side::Buy, 102, 5)); // arrives at 1000 + 1000
    };
    Engine engine(feed, latency(0, 1000, 0), s);
    engine.run();
    ASSERT_EQ(engine.fills().size(), 1u);
    EXPECT_EQ(engine.fills()[0].exch_ts, 2500);
    EXPECT_EQ(engine.fills()[0].qty, 5);
    ASSERT_GE(s.reports.size(), 2u);
    EXPECT_EQ(s.reports[0].report.type, ExecType::Ack);
    EXPECT_EQ(s.reports[0].report.exch_ts, 2000);
}

TEST(Causality, ComputeDelayIsAddedToTheOrderPath) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Sell, 101, 10),
        del(1250, 1), // gone before a 100+200ns order can arrive
        add(9000, 2, Side::Sell, 110, 1),
    });
    for (const Timestamp compute : {Timestamp{0}, Timestamp{200}}) {
        Script s;
        bool sent = false;
        s.md = [&](Context& c, const MarketEvent&) {
            if (!sent) c.send_limit(0, Side::Buy, 101, 10, Tif::IOC);
            sent = true;
        };
        Engine engine(feed, latency(0, 100, 0, compute), s);
        engine.run();
        // 1000 + 100 = 1100 < 1250: filled. 1000 + 200 + 100 = 1300: too late.
        EXPECT_EQ(engine.summary().qty_filled, compute == 0 ? 10 : 0);
        EXPECT_EQ(s.reports[0].report.exch_ts, 1100 + compute);
    }
}

TEST(Causality, FillsAndPositionAreObservedOnlyAfterResponseLatency) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Sell, 101, 10),
        add(1100, 2, Side::Buy, 99, 10),
        add(1200, 3, Side::Buy, 99, 10),
        add(9000, 4, Side::Sell, 110, 1),
    });
    Script s;
    Engine* eng = nullptr;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.order_id == 1) c.send_limit(0, Side::Buy, 101, 4, Tif::IOC); // fills at 1000
        if (e.order_id == 2 || e.order_id == 3) {
            // Truth: we are long 4 since t=1000. Report lands at 1000 + 150.
            EXPECT_EQ(eng->portfolio().account(0).position, 4);
            EXPECT_EQ(c.position(0), c.now() >= 1150 ? 4 : 0);
        }
    };
    Engine engine(feed, latency(0, 0, 150), s);
    eng = &engine;
    engine.run();
    ASSERT_EQ(s.reports.size(), 2u);
    EXPECT_EQ(s.reports[1].report.type, ExecType::Fill);
    EXPECT_EQ(s.reports[1].report.exch_ts, 1000);
    EXPECT_EQ(s.reports[1].now, 1150);
}

TEST(Causality, CancelArrivingAfterFillIsARaceNotARetroactiveRemoval) {
    const FeedData feed = feed_of({
        add(100, 1, Side::Buy, 100, 50),
        add(100, 2, Side::Sell, 105, 50),
        add(2000, 3, Side::Sell, 106, 1), // tick: strategy sends the cancel here
        exec(2400, 1, 5),                 // fills our 102 bid while the cancel is in flight
        add(9000, 4, Side::Sell, 110, 1),
    });
    Script s;
    OrderId id = 0;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.type == MdType::Add && e.order_id == 1)
            id = c.send_limit(0, Side::Buy, 102, 5).id; // arrives at 800
        if (e.order_id == 3) {
            EXPECT_TRUE(c.cancel(id));  // leaves at 2000, arrives at 2700
            EXPECT_FALSE(c.cancel(id)); // one cancel in flight is enough
        }
    };
    Engine engine(feed, latency(0, 700, 0), s);
    engine.run();

    // Order rested from 800. Fill at 2400 stands; the cancel (arrives 2700) loses.
    ASSERT_EQ(engine.fills().size(), 1u);
    EXPECT_EQ(engine.fills()[0].exch_ts, 2400);
    EXPECT_EQ(engine.portfolio().account(0).position, 5);
    EXPECT_EQ(engine.summary().cancels_sent, 1u);
    EXPECT_EQ(engine.summary().cancel_rejects, 1u);
    ASSERT_EQ(s.reports.size(), 3u);
    EXPECT_EQ(s.reports[0].report.type, ExecType::Ack);
    EXPECT_EQ(s.reports[1].report.type, ExecType::Fill);
    EXPECT_EQ(s.reports[2].report.type, ExecType::CancelRejected);
    EXPECT_EQ(s.reports[2].report.reason, RejectReason::TooLateToCancel);
    EXPECT_EQ(s.reports[2].report.exch_ts, 2700);
    EXPECT_EQ(engine.oms().position(0), 5);
    EXPECT_EQ(engine.oms().open_orders(), 0u);
}

TEST(Causality, CancelThatWinsTheRacePreventsTheFill) {
    const FeedData feed = feed_of({
        add(100, 1, Side::Buy, 100, 50),
        add(100, 2, Side::Sell, 105, 50),
        add(2000, 3, Side::Sell, 106, 1),
        exec(2800, 1, 5), // after the cancel landed at 2700
        add(9000, 4, Side::Sell, 110, 1),
    });
    Script s;
    OrderId id = 0;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.type == MdType::Add && e.order_id == 1) id = c.send_limit(0, Side::Buy, 102, 5).id;
        if (e.order_id == 3) c.cancel(id);
    };
    Engine engine(feed, latency(0, 700, 0), s);
    engine.run();
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(engine.summary().cancel_rejects, 0u);
    EXPECT_EQ(s.reports.back().report.type, ExecType::Cancelled);
    EXPECT_EQ(s.reports.back().report.exch_ts, 2700);
}

TEST(Causality, JitterNeverReordersAPath) {
    std::vector<MarketEvent> evs;
    for (int i = 0; i < 500; ++i)
        evs.push_back(add(1000 + i * 10, static_cast<OrderId>(i + 1), Side::Buy, 100 + i % 7, 1));
    evs.push_back(add(1'000'000, 9999, Side::Sell, 500, 1));
    const FeedData feed = feed_of(evs);
    Script s;
    std::vector<OrderId> order;
    Timestamp last = 0;
    s.md = [&](Context& c, const MarketEvent& e) {
        EXPECT_GE(c.now(), last);
        EXPECT_GE(c.now(), e.ts + 100); // never earlier than the base delay
        last = c.now();
        order.push_back(e.order_id);
    };
    EngineConfig cfg = latency(100, 0, 0);
    cfg.latency.md_jitter_ns = 5000; // 500x the inter-event gap
    Engine engine(feed, cfg, s);
    engine.run();
    ASSERT_EQ(order.size(), 500u);
    for (std::size_t i = 0; i < order.size(); ++i) ASSERT_EQ(order[i], i + 1);
}

TEST(Causality, FeedEventsAtATimestampPrecedeOrdersArrivingAtThatTimestamp) {
    // Documented tie-break: exchange state at time t includes every historical
    // event stamped t before any simulated order arriving at t is matched.
    const FeedData feed = feed_of({
        add(500, 1, Side::Sell, 101, 10),
        del(1000, 1), // the ask is pulled at exactly the order's arrival time
        add(9000, 2, Side::Sell, 110, 1),
    });
    Script s;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.type == MdType::Add && e.order_id == 1)
            c.send_limit(0, Side::Buy, 101, 10, Tif::IOC); // sent 500, arrives 1000
    };
    Engine engine(feed, latency(0, 500, 0), s);
    engine.run();
    EXPECT_EQ(engine.summary().qty_filled, 0);
    ASSERT_FALSE(s.reports.empty());
    EXPECT_EQ(s.reports[0].report.exch_ts, 1000);
}

TEST(Causality, OrderArrivingOneNanosecondEarlierDoesTrade) {
    const FeedData feed = feed_of({
        add(500, 1, Side::Sell, 101, 10),
        del(1000, 1),
        add(9000, 2, Side::Sell, 110, 1),
    });
    Script s;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.type == MdType::Add && e.order_id == 1)
            c.send_limit(0, Side::Buy, 101, 10, Tif::IOC); // arrives 999
    };
    Engine engine(feed, latency(0, 499, 0), s);
    engine.run();
    EXPECT_EQ(engine.summary().qty_filled, 10);
}
