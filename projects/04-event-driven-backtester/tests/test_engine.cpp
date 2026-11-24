// Scheduler, determinism, risk, accounting and reporting at engine level.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "hftbt/config.hpp"
#include "hftbt/strategies.hpp"
#include "hftbt/synth.hpp"
#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

namespace {

EngineConfig realistic() {
    EngineConfig c;
    c.latency.md_delay_ns = 40'000;
    c.latency.md_jitter_ns = 15'000;
    c.latency.compute_delay_ns = 5'000;
    c.latency.order_delay_ns = 80'000;
    c.latency.order_jitter_ns = 30'000;
    c.latency.response_delay_ns = 80'000;
    c.latency.response_jitter_ns = 30'000;
    c.fees.maker_bps = -0.2;
    c.fees.taker_bps = 0.5;
    c.risk.max_position = 200;
    c.sample_interval_ns = 10'000'000;
    c.markout_horizons_ns = {1'000'000, 50'000'000};
    c.seed = 99;
    return c;
}

// Trades often enough on a short synthetic feed for the assertions to bite.
ImbalanceParams eager_taker() {
    ImbalanceParams p;
    p.threshold = 0.3;
    p.cooldown_ns = 100'000;
    p.max_hold_ns = 5'000'000;
    return p;
}

struct RunResult {
    Summary summary;
    std::vector<FillRecord> fills;
    std::vector<EquitySample> equity;
};

template <typename S, typename P>
RunResult run_with(const FeedData& feed, const EngineConfig& cfg, const P& params) {
    S strat(params);
    Engine engine(feed, cfg, strat);
    engine.run();
    return {engine.summary(), engine.fills(), engine.equity_curve()};
}

void expect_identical(const RunResult& a, const RunResult& b) {
    EXPECT_EQ(a.summary.event_hash, b.summary.event_hash);
    EXPECT_EQ(a.summary.dispatched_events, b.summary.dispatched_events);
    EXPECT_EQ(a.summary.pnl_net, b.summary.pnl_net); // bitwise, not approximately
    ASSERT_EQ(a.fills.size(), b.fills.size());
    for (std::size_t i = 0; i < a.fills.size(); ++i) {
        EXPECT_EQ(a.fills[i].exch_ts, b.fills[i].exch_ts);
        EXPECT_EQ(a.fills[i].order_id, b.fills[i].order_id);
        EXPECT_EQ(a.fills[i].price, b.fills[i].price);
        EXPECT_EQ(a.fills[i].qty, b.fills[i].qty);
    }
    ASSERT_EQ(a.equity.size(), b.equity.size());
    for (std::size_t i = 0; i < a.equity.size(); ++i)
        EXPECT_EQ(a.equity[i].equity, b.equity[i].equity);
}

} // namespace

TEST(Determinism, SameInputSeedConfigGivesIdenticalEventsAndPnl) {
    const FeedData feed = generate_l3({.seed = 7, .events = 60000, .instruments = 2});
    const EngineConfig cfg = realistic();
    const RunResult a = run_with<MarketMaker>(feed, cfg, MarketMakerParams{});
    const RunResult b = run_with<MarketMaker>(feed, cfg, MarketMakerParams{});
    ASSERT_GT(a.fills.size(), 10u) << "test is vacuous without fills";
    expect_identical(a, b);

    const RunResult c = run_with<ImbalanceTaker>(feed, cfg, eager_taker());
    const RunResult d = run_with<ImbalanceTaker>(feed, cfg, eager_taker());
    ASSERT_GT(c.fills.size(), 10u);
    expect_identical(c, d);
}

TEST(Determinism, RegeneratedFeedAndReparsedFeedGiveTheSameRun) {
    const SynthConfig sc{.seed = 8, .events = 30000};
    const FeedData a = generate_l3(sc);
    FeedData b;
    std::string err;
    const auto bin = to_bin(generate_l3(sc));
    ASSERT_TRUE(parse_bin({bin.data(), bin.size()}, b, err)) << err;
    expect_identical(run_with<MarketMaker>(a, realistic(), MarketMakerParams{}),
                     run_with<MarketMaker>(b, realistic(), MarketMakerParams{}));
}

TEST(Determinism, SeedOnlyMattersThroughJitter) {
    const FeedData feed = generate_l3({.seed = 9, .events = 40000});
    EngineConfig a = realistic();
    EngineConfig b = a;
    b.seed = a.seed + 1;
    EXPECT_NE(run_with<MarketMaker>(feed, a, MarketMakerParams{}).summary.event_hash,
              run_with<MarketMaker>(feed, b, MarketMakerParams{}).summary.event_hash);
    a.latency.md_jitter_ns = a.latency.order_jitter_ns = a.latency.response_jitter_ns = 0;
    b.latency = a.latency;
    expect_identical(run_with<MarketMaker>(feed, a, MarketMakerParams{}),
                     run_with<MarketMaker>(feed, b, MarketMakerParams{}));
}

TEST(Scheduler, FifoAndHeapMarketDataQueuesAreEquivalent) {
    const FeedData l3 = generate_l3({.seed = 10, .events = 50000, .instruments = 3});
    const FeedData l2 = l3_to_l2(l3);
    for (const FeedData* feed : {&l3, &l2}) {
        for (const Timestamp md_delay : {Timestamp{0}, Timestamp{40'000}, Timestamp{5'000'000}}) {
            EngineConfig fifo = realistic();
            fifo.latency.md_delay_ns = md_delay;
            EngineConfig heap = fifo;
            heap.md_fifo = false;
            const RunResult a = run_with<MarketMaker>(*feed, fifo, MarketMakerParams{});
            const RunResult b = run_with<MarketMaker>(*feed, heap, MarketMakerParams{});
            ASSERT_GT(a.fills.size(), 0u);
            expect_identical(a, b);
        }
    }
}

TEST(Scheduler, TimersFireInTimeOrderAndNeverInThePast) {
    const FeedData feed =
        feed_of({add(1000, 1, Side::Buy, 100, 1), add(5000, 2, Side::Buy, 100, 1)});
    Script s;
    std::vector<std::pair<Timestamp, std::uint64_t>> fired;
    std::uint64_t t_late = 0, t_early = 0, t_past = 0, t_beyond = 0;
    s.start = [&](Context& c) {
        t_late = c.set_timer(3000);
        t_early = c.set_timer(2000);
        t_past = c.set_timer(5);       // clamped to now (1000)
        t_beyond = c.set_timer(99999); // after the run ends: never fires
    };
    s.timer = [&](Context& c, std::uint64_t id) {
        fired.emplace_back(c.now(), id);
        if (id == t_early) c.set_timer(c.now()); // re-arm at "now" is allowed
    };
    Engine engine(feed, {}, s);
    engine.run();
    ASSERT_EQ(fired.size(), 4u);
    EXPECT_EQ(fired[0], std::make_pair(Timestamp{1000}, t_past));
    EXPECT_EQ(fired[1], std::make_pair(Timestamp{2000}, t_early));
    EXPECT_EQ(fired[2].first, 2000);
    EXPECT_EQ(fired[3], std::make_pair(Timestamp{3000}, t_late));
    (void)t_beyond;
}

TEST(Scheduler, EndTsStopsTheRunAndStepIsSingleEvent) {
    const FeedData feed = generate_l3({.seed = 12, .events = 5000});
    EngineConfig cfg;
    cfg.end_ts = feed.events[2500].ts;
    NoopStrategy noop;
    Engine engine(feed, cfg, noop);
    engine.start();
    std::uint64_t steps = 0;
    Timestamp last = 0;
    while (engine.step()) {
        ++steps;
        ASSERT_GE(engine.now(), last) << "time went backwards";
        last = engine.now();
    }
    engine.finish();
    EXPECT_EQ(steps, engine.summary().dispatched_events);
    EXPECT_LE(engine.summary().end_ts, cfg.end_ts);
    EXPECT_LT(engine.summary().feed_events, 5000u);
    EXPECT_GE(engine.summary().feed_events, 2501u);
    EXPECT_FALSE(engine.step());
}

TEST(Scheduler, TradeStartSuppressesCallbacksButStillBuildsTheBook) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 10),
        add(2000, 2, Side::Sell, 102, 10),
        add(3000, 3, Side::Buy, 101, 10),
        add(4000, 4, Side::Sell, 103, 10),
    });
    EngineConfig cfg;
    cfg.trade_start_ts = 2500;
    Script s;
    std::vector<OrderId> seen;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (seen.empty()) { // first callback already sees the warmed-up book
            EXPECT_EQ(c.book(0).best_bid()->price, 101);
            EXPECT_EQ(c.book(0).best_ask()->price, 102);
        }
        seen.push_back(e.order_id);
    };
    Engine engine(feed, cfg, s);
    engine.run();
    EXPECT_EQ(seen, (std::vector<OrderId>{3, 4}));
}

TEST(Scheduler, EmptyFeedIsANoOp) {
    const FeedData feed = feed_of({});
    NoopStrategy noop;
    Engine engine(feed, {}, noop);
    engine.run();
    EXPECT_EQ(engine.summary().feed_events, 0u);
    EXPECT_EQ(engine.summary().pnl_net, 0.0);
}

TEST(Risk, PreTradeLimitsRejectLocallyAndNeverReachTheExchange) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 50),
        add(1000, 2, Side::Sell, 110, 50),
        add(2000, 3, Side::Sell, 111, 1),
        add(9000, 4, Side::Sell, 112, 1),
    });
    EngineConfig cfg;
    cfg.risk.max_order_qty = 10;
    cfg.risk.max_position = 15;
    cfg.risk.max_open_orders = 3;
    Script s;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.order_id == 1) {
            EXPECT_EQ(c.send_limit(0, Side::Buy, 101, 11).reason, RejectReason::RiskMaxOrderQty);
            EXPECT_EQ(c.send_limit(0, Side::Buy, 0, 5).reason, RejectReason::InvalidOrder);
            EXPECT_TRUE(c.send_limit(0, Side::Buy, 101, 10));
            // 10 open + 6 would allow a 16-lot long: over the 15 limit.
            EXPECT_EQ(c.send_limit(0, Side::Buy, 101, 6).reason, RejectReason::RiskMaxPosition);
            EXPECT_TRUE(c.send_limit(0, Side::Buy, 101, 5));
            // The sell side has its own worst case.
            EXPECT_TRUE(c.send_limit(0, Side::Sell, 109, 10));
            EXPECT_EQ(c.send_limit(0, Side::Sell, 109, 1).reason, RejectReason::RiskMaxOpenOrders);
            EXPECT_EQ(c.oms().open_orders(), 3u);
            EXPECT_EQ(c.oms().open_qty(0, Side::Buy), 15);
        }
        if (e.order_id == 3) {
            const OrderId first = 2; // id 1 was consumed by the first risk-rejected attempt
            ASSERT_NE(c.oms().find(first), nullptr);
            EXPECT_EQ(c.oms().find(first)->status, OrderStatus::Live);
            EXPECT_TRUE(c.cancel(first));
            EXPECT_FALSE(c.cancel(424242));
        }
    };
    Engine engine(feed, cfg, s);
    engine.run();
    EXPECT_EQ(engine.summary().orders_sent, 3u);
    EXPECT_EQ(engine.summary().orders_risk_rejected, 4u);
    EXPECT_EQ(engine.summary().qty_submitted, 25);
    EXPECT_EQ(engine.oms().open_orders(), 2u);
    EXPECT_EQ(engine.oms().open_qty(0, Side::Buy), 5);
    EXPECT_EQ(engine.exchange().open_orders(), 2u);
}

TEST(Accounting, CashPlusMarkedInventoryEqualsTrackedEquity) {
    const FeedData feed = generate_l3({.seed = 13, .events = 80000, .instruments = 3});
    for (const bool taker : {false, true}) {
        EngineConfig cfg = realistic();
        cfg.tick_value = 12.5;
        MarketMaker mm{MarketMakerParams{}};
        ImbalanceTaker it{eager_taker()};
        Strategy& strat = taker ? static_cast<Strategy&>(it) : mm;
        Engine engine(feed, cfg, strat);
        engine.run();
        const Portfolio& p = engine.portfolio();
        const Summary& s = engine.summary();
        ASSERT_GT(s.fills, 20u);

        // Two independent conventions agree.
        const double scale = std::max(1.0, s.turnover);
        EXPECT_NEAR(p.equity(), p.pnl_decomposed(), 1e-9 * scale);
        EXPECT_NEAR(s.pnl_net, s.realized + s.unrealized - s.fees, 1e-9 * scale);
        EXPECT_NEAR(s.pnl_gross, s.pnl_net + s.fees, 1e-9 * scale);

        // And both agree with a from-scratch replay of the fill tape.
        std::vector<double> cash(3, 0.0);
        std::vector<Qty> pos(3, 0);
        Qty qty = 0;
        for (const FillRecord& f : engine.fills()) {
            cash[f.instrument] -=
                sign(f.side) * static_cast<double>(f.price * f.qty) * 12.5 + f.fee;
            pos[f.instrument] += sign(f.side) * f.qty;
            qty += f.qty;
        }
        double equity = 0.0;
        for (InstrumentId i = 0; i < 3; ++i) {
            EXPECT_EQ(pos[i], p.account(i).position);
            EXPECT_EQ(pos[i], s.final_positions[i]);
            equity += cash[i] + static_cast<double>(pos[i]) * p.account(i).mark * 12.5;
        }
        EXPECT_NEAR(equity, s.pnl_net, 1e-9 * scale);
        EXPECT_EQ(qty, s.qty_filled);
        EXPECT_EQ(s.fills, s.maker_fills + s.taker_fills);
        EXPECT_LE(s.qty_filled, s.qty_submitted);
        EXPECT_NEAR(s.fill_ratio,
                    static_cast<double>(s.qty_filled) / static_cast<double>(s.qty_submitted),
                    1e-12);
        EXPECT_NEAR(engine.equity_curve().back().equity, s.pnl_net, 1e-9 * scale);
        EXPECT_GE(s.max_drawdown, 0.0);
        EXPECT_GE(s.max_gross_exposure, s.avg_gross_exposure);
    }
}

TEST(Accounting, FeesAndLatencyChangeTheResult) {
    // Guards against a config field silently not being wired through.
    const FeedData feed = generate_l3({.seed = 14, .events = 60000});
    const EngineConfig base = realistic();
    const RunResult r0 = run_with<ImbalanceTaker>(feed, base, eager_taker());
    ASSERT_GT(r0.fills.size(), 10u);

    EngineConfig fees = base;
    fees.fees.taker_bps = 5.0;
    const RunResult r1 = run_with<ImbalanceTaker>(feed, fees, eager_taker());
    EXPECT_EQ(r1.fills.size(), r0.fills.size());       // fees do not change behaviour...
    EXPECT_LT(r1.summary.pnl_net, r0.summary.pnl_net); // ...only what it costs
    EXPECT_GT(r1.summary.fees, r0.summary.fees);

    EngineConfig slow = base;
    slow.latency.order_delay_ns = 20'000'000;
    EXPECT_NE(run_with<ImbalanceTaker>(feed, slow, eager_taker()).summary.event_hash,
              r0.summary.event_hash);

    EngineConfig slip = base;
    slip.exchange.taker_slippage_ticks = 3;
    EXPECT_LT(run_with<ImbalanceTaker>(feed, slip, eager_taker()).summary.pnl_net,
              r0.summary.pnl_net);
}

TEST(Markout, MeasuresMidMoveAfterTheFill) {
    const FeedData feed = feed_of({
        add(1000, 1, Side::Buy, 100, 10),
        add(1000, 2, Side::Sell, 102, 10), // mid 101
        add(1500, 3, Side::Buy, 101, 10),  // mid 101.5 at +500
        del(1900, 3),
        del(1950, 1),
        add(1960, 4, Side::Buy, 96, 10), // mid 99 at +1000
        add(9000, 5, Side::Sell, 200, 1),
    });
    EngineConfig cfg;
    cfg.markout_horizons_ns = {500, 1000, 1'000'000};
    Script s;
    s.md = [&](Context& c, const MarketEvent& e) {
        if (e.order_id == 2 && e.type == MdType::Add) c.send_limit(0, Side::Buy, 102, 4, Tif::IOC);
    };
    Engine engine(feed, cfg, s);
    engine.run();
    const Summary& sum = engine.summary();
    ASSERT_EQ(sum.taker_fills, 1u); // bought 4 @ 102 at t=1000
    ASSERT_EQ(sum.markouts.size(), 3u);
    EXPECT_EQ(sum.markouts[0].taker_qty, 4);
    EXPECT_DOUBLE_EQ(sum.markouts[0].taker_sum, 4 * (101.5 - 102.0));
    EXPECT_DOUBLE_EQ(sum.markouts[1].taker_sum, 4 * (99.0 - 102.0));
    EXPECT_EQ(sum.markouts[2].taker_qty, 0); // horizon beyond the end of the data
    EXPECT_EQ(sum.markouts_unresolved, 1u);
}

TEST(Reports, FilesAreWrittenAndConsistentWithTheRun) {
    const FeedData feed = generate_l3({.seed = 15, .events = 30000});
    EngineConfig cfg = realistic();
    const auto dir = std::filesystem::temp_directory_path() / "hftbt_report_test";
    std::filesystem::remove_all(dir);
    cfg.event_log_path = (dir / "events.log").string();
    std::filesystem::create_directories(dir);
    MarketMaker mm{MarketMakerParams{}};
    Engine engine(feed, cfg, mm);
    engine.run();
    std::string err;
    ASSERT_TRUE(write_reports(engine, dir.string(), err)) << err;

    const auto lines = [&](const char* name) {
        std::ifstream in(dir / name);
        std::size_t n = 0;
        for (std::string l; std::getline(in, l);) ++n;
        return n;
    };
    EXPECT_EQ(lines("fills.csv"), engine.fills().size() + 1);
    EXPECT_EQ(lines("equity.csv"), engine.equity_curve().size() + 1);
    EXPECT_GT(lines("events.log"), feed.events.size());
    std::ifstream in(dir / "summary.json");
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    EXPECT_NE(json.find("\"event_hash\""), std::string::npos);
    EXPECT_NE(json.find("\"SYN0\""), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(Config, ParsesOverridesAndFlagsProblems) {
    Config c;
    std::string err;
    ASSERT_TRUE(c.parse("# comment\n[run]\nseed = 5\nmarkout_horizons_ns = 10, 20\n"
                        "[latency]\nmd_delay_ns = 100 ; trailing comment\n"
                        "[exchange]\nqueue_model = optimistic\n"
                        "[strategy]\nname = market_maker\nhalf_spread_ticks = 3\ntypo_key = 1\n"
                        "[fees]\nmaker_bps = abc\n",
                        err))
        << err;
    ASSERT_TRUE(c.set("latency.md_delay_ns=250", err));
    EXPECT_FALSE(c.set("no_equals_sign", err));
    const EngineConfig e = make_engine_config(c);
    EXPECT_EQ(e.seed, 5u);
    EXPECT_EQ(e.latency.md_delay_ns, 250);
    EXPECT_EQ(e.exchange.queue_model, QueueModel::Optimistic);
    EXPECT_EQ(e.markout_horizons_ns, (std::vector<Timestamp>{10, 20}));
    EXPECT_NE(make_strategy(c, err), nullptr);

    const auto problems = c.problems();
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_NE(problems[0].find("fees.maker_bps"), std::string::npos);
    EXPECT_NE(problems[1].find("strategy.typo_key"), std::string::npos);

    Config bad;
    EXPECT_FALSE(bad.parse("[run]\njust a line\n", err));
    ASSERT_TRUE(bad.set("strategy.name=nope", err));
    EXPECT_EQ(make_strategy(bad, err), nullptr);
}
