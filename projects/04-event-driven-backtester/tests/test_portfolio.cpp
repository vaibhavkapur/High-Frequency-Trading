#include <gtest/gtest.h>

#include "hftbt/portfolio.hpp"

using namespace hftbt;

namespace {
ExecReport fill(Side s, Price px, Qty q, bool maker = false) {
    ExecReport r;
    r.type = ExecType::Fill;
    r.side = s;
    r.price = px;
    r.qty = q;
    r.maker = maker;
    return r;
}
} // namespace

TEST(Portfolio, RoundTripRealizesPnl) {
    Portfolio p(1, {}, 1.0);
    p.on_fill(fill(Side::Buy, 100, 10));
    p.on_fill(fill(Side::Sell, 103, 10));
    EXPECT_EQ(p.account(0).position, 0);
    EXPECT_DOUBLE_EQ(p.account(0).realized, 30.0);
    EXPECT_DOUBLE_EQ(p.equity(), 30.0);
    EXPECT_DOUBLE_EQ(p.pnl_decomposed(), 30.0);
    EXPECT_DOUBLE_EQ(p.total_notional(), 2030.0);
}

TEST(Portfolio, AverageCostAndFlipThroughZero) {
    Portfolio p(1, {}, 1.0);
    p.on_fill(fill(Side::Buy, 100, 4));
    p.on_fill(fill(Side::Buy, 110, 4));
    EXPECT_DOUBLE_EQ(p.account(0).avg_cost, 105.0);
    p.on_fill(fill(Side::Sell, 115, 11)); // closes 8, opens short 3 at 115
    EXPECT_EQ(p.account(0).position, -3);
    EXPECT_DOUBLE_EQ(p.account(0).realized, 80.0);
    EXPECT_DOUBLE_EQ(p.account(0).avg_cost, 115.0);
    p.set_mark(0, 112.0);
    EXPECT_DOUBLE_EQ(p.unrealized(0), 9.0);
    EXPECT_DOUBLE_EQ(p.equity(), 89.0);
    EXPECT_DOUBLE_EQ(p.pnl_decomposed(), 89.0);
    EXPECT_DOUBLE_EQ(p.gross_exposure(), 336.0);
}

TEST(Portfolio, MakerRebateAndTakerFee) {
    Portfolio p(1, {.maker_bps = -1.0, .taker_bps = 5.0}, 2.0); // tick worth 2 currency
    const double rebate = p.on_fill(fill(Side::Buy, 1000, 10, true));
    EXPECT_DOUBLE_EQ(rebate, -2.0); // 20000 notional * -1bp
    const double fee = p.on_fill(fill(Side::Sell, 1000, 10, false));
    EXPECT_DOUBLE_EQ(fee, 10.0);
    EXPECT_DOUBLE_EQ(p.total_fees(), 8.0);
    EXPECT_DOUBLE_EQ(p.equity(), -8.0); // flat price, paid net fees
    EXPECT_DOUBLE_EQ(p.pnl_decomposed(), -8.0);
    EXPECT_EQ(p.account(0).maker_fills, 1u);
    EXPECT_EQ(p.account(0).taker_fills, 1u);
}

TEST(Portfolio, FirstFillSetsMarkSoEquityStartsAtMinusFees) {
    Portfolio p(2, {.maker_bps = 0.0, .taker_bps = 10.0}, 1.0);
    ExecReport f = fill(Side::Sell, 500, 2);
    f.instrument = 1;
    p.on_fill(f);
    EXPECT_DOUBLE_EQ(p.equity(), -1.0);
    EXPECT_DOUBLE_EQ(p.pnl_decomposed(), -1.0);
}
