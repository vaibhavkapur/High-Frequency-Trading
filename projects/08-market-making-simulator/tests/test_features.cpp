#include "mmsim/features.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/strategy.hpp"
#include "test_framework.hpp"

using namespace mmsim;

TEST(MicropriceAndImbalanceExactValues) {
    FlatArrayBook book;
    FeatureConfig fcfg{};
    fcfg.imbalance_depth = 3;
    FeatureEngine fe(fcfg);

    // Bid: 10000 x 300, 9999 x 200; Ask: 10002 x 100, 10003 x 100
    MarketEvent e1{0, 1, MarketEventType::BookDelta, Side::Bid, 10000, 300};
    book.apply_delta(e1.side, e1.price_ticks, e1.qty);
    fe.on_event(e1, book);

    MarketEvent e2{1000, 2, MarketEventType::BookDelta, Side::Bid, 9999, 200};
    book.apply_delta(e2.side, e2.price_ticks, e2.qty);
    fe.on_event(e2, book);

    MarketEvent e3{2000, 3, MarketEventType::BookDelta, Side::Ask, 10002, 100};
    book.apply_delta(e3.side, e3.price_ticks, e3.qty);
    fe.on_event(e3, book);

    MarketEvent e4{3000, 4, MarketEventType::BookDelta, Side::Ask, 10003, 100};
    book.apply_delta(e4.side, e4.price_ticks, e4.qty);
    const BookSignals sig = fe.on_event(e4, book);

    EXPECT_TRUE(sig.valid);
    EXPECT_NEAR(sig.mid_ticks, 10001.0, 1e-9);
    // microprice = (10000 * 100 + 10002 * 300) / 400 = 10001.5
    EXPECT_NEAR(sig.microprice_ticks, 10001.5, 1e-9);
    // L1 imbalance = (300 - 100) / 400 = +0.5
    EXPECT_NEAR(sig.l1_imbalance, 0.5, 1e-9);
    // Weighted top-K: wb = 300/1 + 200/2 = 400, wa = 100/1 + 100/2 = 150 -> (400-150)/550
    EXPECT_NEAR(sig.topk_imbalance, 250.0 / 550.0, 1e-9);
}

TEST(OfiAndRealizedVolRespondToFlowAndMidMoves) {
    FlatArrayBook book;
    FeatureConfig fcfg{};
    fcfg.ofi_halflife_sec = 0.5;
    fcfg.vol_halflife_sec = 1.0;
    FeatureEngine fe(fcfg);

    auto apply = [&](TimestampNs ts, MarketEventType type, Side side, PriceTicks px, Qty q) {
        MarketEvent ev{ts, 0, type, side, px, q};
        if (type == MarketEventType::BookDelta) {
            book.apply_delta(side, px, q);
        }
        return fe.on_event(ev, book);
    };

    apply(0, MarketEventType::BookDelta, Side::Bid, 10000, 200);
    const BookSignals s0 = apply(0, MarketEventType::BookDelta, Side::Ask, 10002, 200);
    const double vol0 = s0.realized_vol_ticks_per_sqrt_sec;

    // Aggressive buy trades push OFI strictly positive.
    const BookSignals s1 = apply(10 * kNsPerMs, MarketEventType::Trade, Side::Bid, 10002, 180);
    EXPECT_TRUE(s1.short_horizon_ofi > 0.0);

    // Mid-price jump increases realized volatility.
    apply(100 * kNsPerMs, MarketEventType::BookDelta, Side::Ask, 10002, 0);
    apply(100 * kNsPerMs, MarketEventType::BookDelta, Side::Ask, 10006, 200);
    const BookSignals s2 = apply(100 * kNsPerMs, MarketEventType::BookDelta, Side::Bid, 10004, 200);
    EXPECT_TRUE(s2.realized_vol_ticks_per_sqrt_sec > vol0);
}

TEST(InventoryAwareStrategySkewsQuotesAgainstInventory) {
    StrategyConfig scfg{};
    scfg.type = StrategyType::InventoryAware;
    scfg.risk_aversion = 2.0;
    scfg.base_half_spread_ticks = 1.0;
    scfg.post_only = false;
    InventoryAwareStrategy strat(scfg);

    BookSignals sig{};
    sig.valid = true;
    sig.tob = TopOfBook{10000, 10002, 200, 200};
    sig.mid_ticks = 10001.0;
    sig.microprice_ticks = 10001.0;
    sig.spread_ticks = 2;
    sig.realized_vol_ticks_per_sqrt_sec = 1.5;

    const QuoteDecision q_flat = strat.compute_quotes(sig, StrategyState{0, 500, 0.0});
    const QuoteDecision q_long = strat.compute_quotes(sig, StrategyState{300, 500, 0.0});
    const QuoteDecision q_short = strat.compute_quotes(sig, StrategyState{-300, 500, 0.0});

    // Long inventory lowers reservation price -> lower bid & lower ask, smaller bid size.
    EXPECT_TRUE(q_long.reservation_price_ticks < q_flat.reservation_price_ticks);
    EXPECT_LE(q_long.bid_price_ticks, q_flat.bid_price_ticks);
    EXPECT_LE(q_long.ask_price_ticks, q_flat.ask_price_ticks);
    EXPECT_TRUE(q_long.bid_qty < q_flat.bid_qty);

    // Short inventory raises reservation price -> higher bid & higher ask, smaller ask size.
    EXPECT_TRUE(q_short.reservation_price_ticks > q_flat.reservation_price_ticks);
    EXPECT_GE(q_short.bid_price_ticks, q_flat.bid_price_ticks);
    EXPECT_GE(q_short.ask_price_ticks, q_flat.ask_price_ticks);
    EXPECT_TRUE(q_short.ask_qty < q_flat.ask_qty);
}

MMSIM_TEST_MAIN()
