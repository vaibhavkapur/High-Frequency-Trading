#include <gtest/gtest.h>

#include "ouch/risk.hpp"

using namespace ouch;

namespace {

const Symbol kSym("AAPL");
constexpr std::uint32_t kRef = 1'500'000;  // $150.0000

RiskEngine engine(RiskLimits l = {}) {
  RiskEngine r(l);
  r.set_reference_price(kSym, kRef);
  return r;
}

}  // namespace

TEST(Risk, AcceptsOrdinaryOrder) {
  EXPECT_EQ(engine().check('B', kSym, 100, kRef, {}), RiskReject::None);
  EXPECT_EQ(engine().check('S', kSym, 100, kRef, {}), RiskReject::None);
}

TEST(Risk, ZeroAndMaxQuantity) {
  RiskLimits l;
  l.max_order_qty = 500;
  const auto r = engine(l);
  EXPECT_EQ(r.check('B', kSym, 0, kRef, {}), RiskReject::ZeroQuantity);
  EXPECT_EQ(r.check('B', kSym, 500, kRef, {}), RiskReject::None);
  EXPECT_EQ(r.check('B', kSym, 501, kRef, {}), RiskReject::MaxOrderQty);
}

TEST(Risk, MaxNotionalBoundaryIsInclusive) {
  RiskLimits l;
  l.max_order_qty = 1'000'000;
  l.max_notional = 150'000ULL * kPriceScale;  // $150,000
  const auto r = engine(l);
  EXPECT_EQ(r.check('B', kSym, 1000, kRef, {}), RiskReject::None);  // exactly $150,000
  EXPECT_EQ(r.check('B', kSym, 1001, kRef, {}), RiskReject::MaxNotional);
}

TEST(Risk, NotionalDoesNotOverflow32Bits) {
  RiskLimits l;
  l.max_order_qty = 999'999;
  l.max_notional = UINT64_MAX;
  l.require_reference_price = false;  // no reference price, so no price band
  l.max_long = INT64_MAX / 2;
  const RiskEngine r(l);
  // 999,999 shares at $199,999.99 overflows 32 and 48 bits but not 64.
  EXPECT_EQ(r.check('B', kSym, 999'999, kMaxLimitPrice, {}), RiskReject::None);
}

TEST(Risk, PriceBand) {
  RiskLimits l;
  l.price_band_bps = 500;  // +/- 5% => [142.50, 157.50]
  const auto r = engine(l);
  EXPECT_EQ(r.check('B', kSym, 100, 1'425'000, {}), RiskReject::None);
  EXPECT_EQ(r.check('B', kSym, 100, 1'575'000, {}), RiskReject::None);
  EXPECT_EQ(r.check('B', kSym, 100, 1'424'999, {}), RiskReject::PriceBand);
  EXPECT_EQ(r.check('S', kSym, 100, 1'575'001, {}), RiskReject::PriceBand);
}

TEST(Risk, ReferencePriceRequirement) {
  RiskEngine r;  // no reference prices at all
  EXPECT_EQ(r.check('B', kSym, 100, kRef, {}), RiskReject::NoReferencePrice);
  RiskLimits l;
  l.require_reference_price = false;
  r.set_limits(l);
  EXPECT_EQ(r.check('B', kSym, 100, kRef, {}), RiskReject::None);
}

TEST(Risk, MarketOrders) {
  EXPECT_EQ(engine().check('B', kSym, 100, kMarketPrice, {}), RiskReject::MarketOrderNotAllowed);

  RiskLimits l;
  l.allow_market_orders = true;
  l.max_notional = 15'000ULL * kPriceScale;
  EXPECT_EQ(engine(l).check('B', kSym, 100, kMarketPrice, {}), RiskReject::None);
  // Valued at the reference price: 101 * $150 > $15,000.
  EXPECT_EQ(engine(l).check('B', kSym, 101, kMarketPrice, {}), RiskReject::MaxNotional);

  // A market order cannot be valued without a reference price.
  l.require_reference_price = false;
  RiskEngine none(l);
  EXPECT_EQ(none.check('B', kSym, 100, kMarketPrice, {}), RiskReject::NoReferencePrice);
}

TEST(Risk, PositionLimitsCountOpenOrders) {
  RiskLimits l;
  l.max_long = 1000;
  l.max_short = 500;
  const auto r = engine(l);

  SymbolExposure e;
  e.position = 600;
  e.open_buy = 300;
  EXPECT_EQ(r.check('B', kSym, 100, kRef, e), RiskReject::None);  // worst case exactly 1000
  EXPECT_EQ(r.check('B', kSym, 101, kRef, e), RiskReject::PositionLimit);
  // Selling from a long position is fine until the short limit.
  EXPECT_EQ(r.check('S', kSym, 1100, kRef, e), RiskReject::None);  // 600 - 1100 = -500
  EXPECT_EQ(r.check('S', kSym, 1101, kRef, e), RiskReject::PositionLimit);

  e = {};
  e.position = -200;
  e.open_sell = 200;
  EXPECT_EQ(r.check('S', kSym, 100, kRef, e), RiskReject::None);
  EXPECT_EQ(r.check('S', kSym, 101, kRef, e), RiskReject::PositionLimit);
  // Open sells do not offset the buy-side worst case.
  EXPECT_EQ(r.check('B', kSym, 1200, kRef, e), RiskReject::None);
  EXPECT_EQ(r.check('B', kSym, 1201, kRef, e), RiskReject::PositionLimit);
}

TEST(Risk, ShortSaleSidesAreSells) {
  RiskLimits l;
  l.max_short = 100;
  const auto r = engine(l);
  EXPECT_EQ(r.check('T', kSym, 101, kRef, {}), RiskReject::PositionLimit);
  EXPECT_EQ(r.check('E', kSym, 101, kRef, {}), RiskReject::PositionLimit);
}
