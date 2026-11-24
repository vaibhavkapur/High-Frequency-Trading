#include "mmsim/portfolio.hpp"
#include "mmsim/risk.hpp"
#include "test_framework.hpp"

#include <random>

using namespace mmsim;

TEST(RiskLimitsEnforcedStrictly) {
    RiskConfig rcfg{};
    rcfg.max_abs_position = 300;
    rcfg.max_order_size = 100;
    rcfg.max_gross_notional_dollars = 25000.0; // At $100/sh => max 250 sh worst-case
    rcfg.max_drawdown_dollars = 500.0;
    rcfg.max_open_orders = 2;

    RiskManager rm(rcfg);
    rm.reset(10000.0);

    RiskState st{};
    st.confirmed_position = 150;
    st.open_buy_qty = 100;
    st.open_sell_qty = 0;
    st.open_order_count = 1;
    st.mark_price_dollars = 100.0;
    st.current_net_equity_dollars = 10000.0;

    // Exceeds max_order_size (150 > 100).
    OrderRequest big{1, 0, Side::Bid, 10000, 150, true};
    EXPECT_EQ(static_cast<int>(rm.check_order(big, st)),
              static_cast<int>(RejectReason::MaxOrderSize));

    // Exceeds max_abs_position? 150 + 100 + 60 = 310 > 300.
    OrderRequest over_pos{2, 0, Side::Bid, 10000, 60, true};
    EXPECT_EQ(static_cast<int>(rm.check_order(over_pos, st)),
              static_cast<int>(RejectReason::MaxPosition));

    // Within position (150 + 100 + 10 = 260 <= 300) but exceeds notional (260 * $100 = $26k > $25k).
    OrderRequest over_notional{3, 0, Side::Bid, 10000, 10, true};
    EXPECT_EQ(static_cast<int>(rm.check_order(over_notional, st)),
              static_cast<int>(RejectReason::MaxNotional));

    // Selling 100 reduces exposure -> approved.
    OrderRequest sell_ok{4, 0, Side::Ask, 10001, 100, true};
    EXPECT_EQ(static_cast<int>(rm.check_order(sell_ok, st)),
              static_cast<int>(RejectReason::None));

    // Open order cap reached.
    st.open_order_count = 2;
    EXPECT_EQ(static_cast<int>(rm.check_order(sell_ok, st)),
              static_cast<int>(RejectReason::MaxOpenOrders));

    // Drawdown kill-switch.
    st.open_order_count = 0;
    EXPECT_FALSE(rm.update_equity(1000, 10400.0)); // New peak = 10400
    EXPECT_FALSE(rm.update_equity(2000, 10000.0)); // DD = 400 < 500
    EXPECT_TRUE(rm.update_equity(3000, 9899.0));   // DD = 501 >= 500 -> kill!
    EXPECT_EQ(static_cast<int>(rm.check_order(sell_ok, st)),
              static_cast<int>(RejectReason::MaxDrawdown));
}

TEST(PortfolioPnlReconciliationAcrossFlipsAndFees) {
    Portfolio p(0.01, 50000.0);
    std::mt19937_64 rng(99991);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<PriceTicks> px_dist(9950, 10050);
    std::uniform_int_distribution<Qty> qty_dist(10, 250);
    std::uniform_real_distribution<double> fee_dist(-0.5, 2.0);

    for (int i = 0; i < 2000; ++i) {
        FillEvent f{};
        f.order_id = static_cast<OrderId>(i + 1);
        f.exchange_time_ns = i * 1'000'000;
        f.side = (side_dist(rng) == 0) ? Side::Bid : Side::Ask;
        f.fill_price_ticks = px_dist(rng);
        f.fill_qty = qty_dist(rng);
        f.fee_dollars = fee_dist(rng);
        p.on_fill(f);
        if (i % 17 == 0) {
            p.on_cancel_fee(0.02);
        }

        const double mark = static_cast<double>(px_dist(rng)) * 0.01;
        EXPECT_NEAR(p.reconciliation_error_dollars(mark), 0.0, 1e-7);
    }

    // Exact hand-verified round-trip check.
    Portfolio exact(0.01, 0.0);
    FillEvent b1{1, 100, Side::Bid, 10000, 100, 0, LiquidityFlag::Maker, 10001.0, -0.20, false};
    FillEvent s1{2, 200, Side::Ask, 10004, 150, 0, LiquidityFlag::Maker, 10003.0, 0.30, false};
    exact.on_fill(b1);
    exact.on_fill(s1);
    // Bought 100 @ 100.00, sold 150 @ 100.04 -> realized = 100 * 0.04 = +4.00, pos = -50 @ 100.04
    // Fees = -0.20 + 0.30 = +0.10
    EXPECT_EQ(exact.position(), -50);
    EXPECT_NEAR(exact.realized_pnl_dollars(), 4.00, 1e-9);
    EXPECT_NEAR(exact.total_fees_dollars(), 0.10, 1e-9);
    // Mark @ 100.02 -> unrealized = -50 * (100.02 - 100.04) = +1.00; gross = +5.00; net = +4.90
    EXPECT_NEAR(exact.unrealized_pnl_dollars(100.02), 1.00, 1e-9);
    EXPECT_NEAR(exact.gross_mtm_pnl_dollars(100.02), 5.00, 1e-9);
    EXPECT_NEAR(exact.net_mtm_pnl_dollars(100.02), 4.90, 1e-9);
    EXPECT_NEAR(exact.reconciliation_error_dollars(100.02), 0.0, 1e-9);
}

TEST(AdverseSelectionMarkoutsRespectTimestampSupport) {
    // Session spans [0, 500 ms]: 10 ms and 100 ms supported for early fills, 1 s NOT supported.
    std::vector<MidSample> mids = {
        {0, 10000.0},
        {10 * kNsPerMs, 9999.0},
        {100 * kNsPerMs, 9998.0},
        {500 * kNsPerMs, 9995.0},
    };
    std::vector<FillEvent> fills = {
        // Buy @ 9999 when mid = 10000 at t = 0:
        // captured_half_spread = +1.0 tick
        // at 10ms mid=9999 -> adv_sel = +1.0, realized = 0.0
        // at 100ms mid=9998 -> adv_sel = +2.0, realized = -1.0
        // at 1s -> unsupported (> 500ms)!
        {1, 0, Side::Bid, 9999, 100, 0, LiquidityFlag::Maker, 10000.0, 0.0, false},
        // Fill at t = 450ms -> 10ms supported (460ms <= 500ms), 100ms NOT supported (550ms > 500ms)!
        {2, 450 * kNsPerMs, Side::Ask, 9999, 100, 0, LiquidityFlag::Maker, 9998.0, 0.0, false},
    };

    const std::vector<TimestampNs> horizons = {10 * kNsPerMs, 100 * kNsPerMs, 1 * kNsPerSec};
    std::vector<FillMarkoutRow> rows;
    const auto stats = compute_markouts(fills, mids, 500 * kNsPerMs, horizons, &rows);

    EXPECT_EQ(stats.size(), 3u);
    EXPECT_EQ(stats[0].valid_fills, 2u); // 10 ms supported for both fills
    EXPECT_EQ(stats[1].valid_fills, 1u); // 100 ms supported ONLY for fill #1
    EXPECT_EQ(stats[2].valid_fills, 0u); // 1 s unsupported for both!

    EXPECT_NEAR(stats[1].mean_captured_half_spread_ticks, 1.0, 1e-9);
    EXPECT_NEAR(stats[1].mean_adverse_selection_ticks, 2.0, 1e-9);
    EXPECT_NEAR(stats[1].mean_realized_spread_ticks, -1.0, 1e-9);

    EXPECT_TRUE(rows[0].has_10ms);
    EXPECT_TRUE(rows[0].has_100ms);
    EXPECT_FALSE(rows[0].has_1s);
    EXPECT_TRUE(rows[1].has_10ms);
    EXPECT_FALSE(rows[1].has_100ms);
    EXPECT_FALSE(rows[1].has_1s);
}

MMSIM_TEST_MAIN()
