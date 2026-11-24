#include "mmsim/config.hpp"
#include "mmsim/engine.hpp"
#include "mmsim/feed.hpp"
#include "mmsim/synth.hpp"
#include "test_framework.hpp"

#include <string>

using namespace mmsim;

TEST(StrictCausalityNoFutureInformationUsed) {
    SynthParams sp{};
    sp.seed = 2026;
    sp.regime = MarketRegime::HighVolBurst;
    sp.duration_sec = 10.0;
    const auto base_events = generate_synthetic_session(sp);
    EXPECT_TRUE(base_events.size() > 1000);

    // Mutate all events strictly after cutoff T = 5.0s with extreme price spikes & trades.
    const TimestampNs cutoff_ns = 5 * kNsPerSec;
    auto mutated = base_events;
    for (auto& ev : mutated) {
        if (ev.timestamp_ns > cutoff_ns) {
            ev.price_ticks += 500;
            ev.qty += 1000;
        }
    }

    SimConfig cfg{};
    cfg.strategy.type = StrategyType::InventoryAware;
    SimulatorEngine eng1(cfg);
    SimulatorEngine eng2(cfg);
    const SimResult r1 = eng1.run(base_events, true);
    const SimResult r2 = eng2.run(mutated, true);

    // Every fill at or before cutoff_ns must be 100% identical!
    std::vector<FillEvent> f1, f2;
    for (const auto& r : r1.fills) {
        if (r.fill.exchange_time_ns <= cutoff_ns) f1.push_back(r.fill);
    }
    for (const auto& r : r2.fills) {
        if (r.fill.exchange_time_ns <= cutoff_ns) f2.push_back(r.fill);
    }
    EXPECT_TRUE(!f1.empty());
    EXPECT_EQ(f1.size(), f2.size());
    for (size_t i = 0; i < f1.size(); ++i) {
        EXPECT_EQ(f1[i].order_id, f2[i].order_id);
        EXPECT_EQ(f1[i].exchange_time_ns, f2[i].exchange_time_ns);
        EXPECT_EQ(f1[i].fill_price_ticks, f2[i].fill_price_ticks);
        EXPECT_EQ(f1[i].fill_qty, f2[i].fill_qty);
        EXPECT_EQ(static_cast<int>(f1[i].side), static_cast<int>(f2[i].side));
    }

    // Equity curve points strictly before cutoff_ns must also be identical.
    size_t checked_eq = 0;
    for (size_t i = 0; i < r1.equity_curve.size() && i < r2.equity_curve.size(); ++i) {
        if (r1.equity_curve[i].timestamp_ns > cutoff_ns) {
            break;
        }
        EXPECT_EQ(r1.equity_curve[i].timestamp_ns, r2.equity_curve[i].timestamp_ns);
        EXPECT_EQ(r1.equity_curve[i].position, r2.equity_curve[i].position);
        EXPECT_NEAR(r1.equity_curve[i].cash_dollars, r2.equity_curve[i].cash_dollars, 1e-12);
        ++checked_eq;
    }
    EXPECT_TRUE(checked_eq > 10);
}

TEST(InventoryNeverExceedsLimitsAndPnlReconcilesAcrossAllRegimes) {
    for (MarketRegime reg : {MarketRegime::MeanReverting, MarketRegime::ToxicTrend,
                            MarketRegime::HighVolBurst, MarketRegime::ThinBook}) {
        SynthParams sp{};
        sp.seed = 5000 + static_cast<uint64_t>(reg);
        sp.regime = reg;
        sp.duration_sec = 15.0;
        const auto events = generate_synthetic_session(sp);

        for (StrategyType st : {StrategyType::InventoryAware, StrategyType::NaiveSymmetric}) {
            SimConfig cfg{};
            cfg.strategy.type = st;
            cfg.risk.max_abs_position = 200; // Tight limit to stress-test pre-trade risk
            cfg.risk.max_order_size = 100;
            cfg.latency.md_latency_ns = 200 * kNsPerUs;
            cfg.latency.order_latency_ns = 300 * kNsPerUs;
            cfg.latency.ack_latency_ns = 200 * kNsPerUs;

            SimulatorEngine engine(cfg);
            const SimResult res = engine.run(events, true);

            EXPECT_LE(res.summary.max_abs_position, cfg.risk.max_abs_position);
            EXPECT_NEAR(res.summary.reconciliation_error_dollars, 0.0, 1e-6);
            for (const auto& pt : res.equity_curve) {
                EXPECT_LE(std::abs(pt.position), cfg.risk.max_abs_position);
            }
        }
    }
}

TEST(CancelRaceEndToEndWithNonZeroOrderLatency) {
    // Scenario:
    // t=0: Book initialized Bid 10000 x 50, Ask 10002 x 200. Strategy posts Bid @ 10000 x 100
    //      (arrives exchange at t=2ms, queue_ahead=50).
    // t=10ms: Mid shifts up sharply -> strategy receives MD at t=11ms and immediately sends cancel
    //         for Bid @ 10000 (cancel arrives exchange at t=13ms!).
    // t=12ms: Aggressive seller sweeps 200 @ 10000 on exchange BEFORE cancel arrives at t=13ms!
    // Expect: Bid @ 10000 is FILLED (raced_cancel=true) and cancel returns TooLateToCancel!
    std::vector<MarketEvent> events = {
        {0, 1, MarketEventType::BookDelta, Side::Bid, 10000, 50},
        {0, 2, MarketEventType::BookDelta, Side::Ask, 10002, 200},
        // Mid jumps up at t=10ms (remove old ask first so book stays uncrossed) ->
        // triggers strategy requote/cancel of Bid@10000 at t=11ms (cancel arrives exchange at t=13ms).
        {10 * kNsPerMs, 3, MarketEventType::BookDelta, Side::Ask, 10002, 0},
        {10 * kNsPerMs, 4, MarketEventType::BookDelta, Side::Ask, 10007, 200},
        {10 * kNsPerMs, 5, MarketEventType::BookDelta, Side::Bid, 10005, 200},
        // Aggressive sell sweeps down to 10000 at t=12ms (1ms BEFORE cancel arrives at exchange!).
        {12 * kNsPerMs, 6, MarketEventType::Trade, Side::Ask, 10000, 200},
        {20 * kNsPerMs, 7, MarketEventType::BookDelta, Side::Bid, 10005, 250},
    };

    SimConfig cfg{};
    cfg.strategy.type = StrategyType::NaiveSymmetric;
    cfg.strategy.base_half_spread_ticks = 1.0;
    cfg.strategy.min_quote_lifetime_ns = 0; // Allow immediate cancel when mid moves
    cfg.latency.md_latency_ns = 1 * kNsPerMs;
    cfg.latency.order_latency_ns = 2 * kNsPerMs;
    cfg.latency.ack_latency_ns = 1 * kNsPerMs;

    SimulatorEngine engine(cfg);
    const SimResult res = engine.run(events, true);

    EXPECT_GE(res.summary.fill_count, 1u);
    EXPECT_GE(res.summary.cancel_races_filled, 1u);
    EXPECT_GE(res.summary.cancels_too_late, 1u);
    EXPECT_NEAR(res.summary.reconciliation_error_dollars, 0.0, 1e-8);
}

TEST(FlatArrayAndStdMapProduceBitIdenticalSimulation) {
    SynthParams sp{};
    sp.seed = 8080;
    sp.regime = MarketRegime::ThinBook;
    sp.duration_sec = 10.0;
    const auto events = generate_synthetic_session(sp);

    SimConfig cfg_flat{};
    cfg_flat.book_impl = BookImplType::FlatArray;
    SimConfig cfg_map = cfg_flat;
    cfg_map.book_impl = BookImplType::StdMap;

    const SimResult rf = SimulatorEngine(cfg_flat).run(events, true);
    const SimResult rm = SimulatorEngine(cfg_map).run(events, true);

    EXPECT_EQ(rf.summary.quotes_submitted, rm.summary.quotes_submitted);
    EXPECT_EQ(rf.summary.fill_count, rm.summary.fill_count);
    EXPECT_EQ(rf.summary.final_position, rm.summary.final_position);
    EXPECT_NEAR(rf.summary.net_mtm_pnl_dollars, rm.summary.net_mtm_pnl_dollars, 1e-12);
    EXPECT_NEAR(rf.summary.total_fees_dollars, rm.summary.total_fees_dollars, 1e-12);
}

TEST(ExampleConfigAndTinyCsvReplay) {
    const std::string src_dir = MMSIM_SOURCE_DIR;
    SimConfig cfg{};
    std::string err;
    EXPECT_TRUE(load_config_file(src_dir + "/examples/inventory_aware.cfg", cfg, err));

    std::vector<MarketEvent> events;
    EXPECT_TRUE(load_market_events(src_dir + "/examples/tiny_session.csv", events, nullptr, err));
    EXPECT_EQ(events.size(), 16u);

    const SimResult res = SimulatorEngine(cfg).run(events, true);
    EXPECT_NEAR(res.summary.reconciliation_error_dollars, 0.0, 1e-9);
    EXPECT_LE(res.summary.max_abs_position, cfg.risk.max_abs_position);
}

MMSIM_TEST_MAIN()
