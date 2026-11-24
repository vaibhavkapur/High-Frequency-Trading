#include "mmsim/exchange.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/synth.hpp"
#include "test_framework.hpp"

using namespace mmsim;

TEST(QueuePriorityRequiresDepletionBeforeFill) {
    FlatArrayBook book;
    book.apply_delta(Side::Bid, 10000, 300);
    book.apply_delta(Side::Ask, 10002, 300);

    ExchangeConfig ecfg{};
    ecfg.fill_model = FillModelType::QueuePriority;
    FeeConfig fcfg{};
    fcfg.maker_fee_bps = -0.2;
    SimulatedExchange exch(ecfg, fcfg, 0.01);

    std::vector<FillEvent> fills;
    OrderRequest req{1, 100, Side::Bid, 10000, 100, true};
    const OrderAck ack = exch.on_new_order(100, req, book, fills);
    EXPECT_TRUE(ack.accepted);
    EXPECT_EQ(ack.queue_ahead, 300);
    EXPECT_TRUE(fills.empty());

    // Trade of 200 at 10000 depletes queue_ahead from 300 -> 100; NO fill yet!
    MarketEvent t1{200, 1, MarketEventType::Trade, Side::Ask, 10000, 200};
    exch.on_trade(200, t1, book, fills);
    EXPECT_TRUE(fills.empty());

    // Matching BookDelta for that trade should NOT double-decrement queue_ahead.
    MarketEvent d1{200, 2, MarketEventType::BookDelta, Side::Bid, 10000, 100};
    const Qty old1 = book.apply_delta(d1.side, d1.price_ticks, d1.qty);
    exch.on_book_delta(200, d1, old1, book, fills);
    EXPECT_TRUE(fills.empty());

    // Next trade of 160 at 10000 consumes remaining 100 ahead and fills 60 of our 100!
    MarketEvent t2{300, 3, MarketEventType::Trade, Side::Ask, 10000, 160};
    exch.on_trade(300, t2, book, fills);
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill_qty, 60);
    EXPECT_EQ(fills[0].remaining_qty, 40);
    EXPECT_EQ(fills[0].fill_price_ticks, 10000);
    EXPECT_TRUE(fills[0].fee_dollars < 0.0); // Maker rebate
}

TEST(CancelRaceDoesNotErasePriorFill) {
    FlatArrayBook book;
    book.apply_delta(Side::Bid, 10000, 50);
    book.apply_delta(Side::Ask, 10002, 200);

    ExchangeConfig ecfg{};
    FeeConfig fcfg{};
    SimulatedExchange exch(ecfg, fcfg, 0.01);

    std::vector<FillEvent> fills;
    const OrderAck ack = exch.on_new_order(100, OrderRequest{42, 0, Side::Bid, 10000, 100, true},
                                           book, fills);
    EXPECT_TRUE(ack.accepted);

    // Market sweeps 200 at 10000 at t = 250 -> fills all 100 of order 42 BEFORE cancel arrives at t = 300!
    MarketEvent sweep{250, 1, MarketEventType::Trade, Side::Ask, 10000, 200};
    exch.on_trade(250, sweep, book, fills);
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, 42u);
    EXPECT_EQ(fills[0].fill_qty, 100);

    // Cancel arrives at t = 300 (sent at t = 200 with 100ns latency) -> MUST fail TooLateToCancel.
    const CancelAck cack = exch.on_cancel(300, CancelRequest{42, 200});
    EXPECT_FALSE(cack.cancelled);
    EXPECT_EQ(static_cast<int>(cack.reject_reason),
              static_cast<int>(RejectReason::TooLateToCancel));
}

TEST(PostOnlyRejectionVsTakerExecution) {
    FlatArrayBook book;
    book.apply_delta(Side::Bid, 10000, 200);
    book.apply_delta(Side::Ask, 10002, 200);

    ExchangeConfig ecfg{};
    FeeConfig fcfg{};
    fcfg.taker_fee_bps = 2.0;
    SimulatedExchange exch(ecfg, fcfg, 0.01);

    std::vector<FillEvent> fills;
    // Post-only bid at ask price (10002) is rejected.
    const OrderAck rej = exch.on_new_order(100, OrderRequest{1, 0, Side::Bid, 10002, 100, true},
                                           book, fills);
    EXPECT_FALSE(rej.accepted);
    EXPECT_EQ(static_cast<int>(rej.reject_reason),
              static_cast<int>(RejectReason::CrossesBookPostOnly));
    EXPECT_TRUE(fills.empty());

    // Non-post-only bid at 10002 executes immediately as Taker.
    const OrderAck acc = exch.on_new_order(110, OrderRequest{2, 0, Side::Bid, 10002, 100, false},
                                           book, fills);
    EXPECT_TRUE(acc.accepted);
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_EQ(static_cast<int>(fills[0].liquidity), static_cast<int>(LiquidityFlag::Taker));
    EXPECT_TRUE(fills[0].fee_dollars > 0.0);
}

TEST(WiderSpreadsReduceOrMaintainFillPropensity) {
    // Test monotonicity under BOTH QueuePriority and ProbabilisticTouch fill models.
    SynthParams sp{};
    sp.seed = 777;
    sp.regime = MarketRegime::MeanReverting;
    sp.duration_sec = 10.0;
    const auto events = generate_synthetic_session(sp);

    for (FillModelType fm : {FillModelType::QueuePriority, FillModelType::ProbabilisticTouch}) {
        Qty prev_filled = std::numeric_limits<Qty>::max();
        for (PriceTicks offset : {0, 1, 2, 4}) {
            FlatArrayBook book;
            ExchangeConfig ecfg{};
            ecfg.fill_model = fm;
            ecfg.rng_seed = 12345;
            FeeConfig fcfg{};
            SimulatedExchange exch(ecfg, fcfg, 0.01);

            OrderId oid = 1;
            OrderId active_bid = 0;
            OrderId active_ask = 0;
            Qty total_filled = 0;
            std::vector<FillEvent> fills;

            for (const auto& ev : events) {
                fills.clear();
                if (ev.type == MarketEventType::BookDelta) {
                    const Qty old_q = book.apply_delta(ev.side, ev.price_ticks, ev.qty);
                    exch.on_book_delta(ev.timestamp_ns, ev, old_q, book, fills);
                } else if (ev.type == MarketEventType::Trade) {
                    exch.on_trade(ev.timestamp_ns, ev, book, fills);
                }
                for (const auto& f : fills) {
                    total_filled += f.fill_qty;
                    if (f.remaining_qty == 0) {
                        if (f.order_id == active_bid) active_bid = 0;
                        if (f.order_id == active_ask) active_ask = 0;
                    }
                }
                const TopOfBook tob = book.top_of_book();
                if (tob.valid()) {
                    if (active_bid == 0) {
                        const OrderAck a = exch.on_new_order(
                            ev.timestamp_ns,
                            OrderRequest{oid, ev.timestamp_ns, Side::Bid, tob.best_bid - offset,
                                         100, true},
                            book, fills);
                        if (a.accepted) active_bid = oid;
                        ++oid;
                    }
                    if (active_ask == 0) {
                        const OrderAck a = exch.on_new_order(
                            ev.timestamp_ns,
                            OrderRequest{oid, ev.timestamp_ns, Side::Ask, tob.best_ask + offset,
                                         100, true},
                            book, fills);
                        if (a.accepted) active_ask = oid;
                        ++oid;
                    }
                }
            }
            EXPECT_LE(total_filled, prev_filled);
            prev_filled = total_filled;
        }
    }
}

MMSIM_TEST_MAIN()
