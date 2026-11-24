#include <random>

#include "test_harness.hpp"
#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/types.hpp"

using namespace xarb;

XARB_TEST(DecimalScalingAndFormatting) {
  auto p1 = parse_decimal_scaled("65123.45", 2);
  EXPECT_TRUE(p1.has_value());
  EXPECT_EQ(*p1, 6512345LL);
  EXPECT_EQ(format_scaled(*p1, 2), "65123.45");

  auto p2 = parse_decimal_scaled("0.00125", 6);
  EXPECT_TRUE(p2.has_value());
  EXPECT_EQ(*p2, 1250LL);
  EXPECT_EQ(format_scaled(*p2, 6), "0.001250");

  auto p3 = parse_decimal_scaled("10.006", 2, true);
  EXPECT_TRUE(p3.has_value());
  EXPECT_EQ(*p3, 1001LL);

  auto p4 = parse_decimal_scaled("10.006", 2, false);
  EXPECT_TRUE(p4.has_value());
  EXPECT_EQ(*p4, 1000LL);

  EXPECT_FALSE(parse_decimal_scaled("", 2).has_value());
  EXPECT_FALSE(parse_decimal_scaled("abc", 2).has_value());
  EXPECT_FALSE(parse_decimal_scaled("12.34.56", 2).has_value());
  EXPECT_FALSE(parse_decimal_scaled("999999999999999999999", 2).has_value());
}

XARB_TEST(InstrumentSymbolAndLotNormalization) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  auto b_id = reg.find_native(VenueId::Binance, "BTCUSDT");
  auto c_id = reg.find_native(VenueId::Coinbase, "BTC-USDT");
  auto o_id = reg.find_native(VenueId::Okx, "BTC-USDT");
  EXPECT_TRUE(b_id.has_value());
  EXPECT_TRUE(c_id.has_value());
  EXPECT_TRUE(o_id.has_value());
  EXPECT_EQ(*b_id, *c_id);
  EXPECT_EQ(*c_id, *o_id);

  const auto* spec = reg.get(*b_id);
  EXPECT_TRUE(spec != nullptr);
  EXPECT_EQ(spec->canonical_symbol, "BTC/USDT");
  // Lot step is 10 lots (1e-5 BTC), so 0.000017 BTC snaps down to 10 lots.
  auto q = spec->normalize_qty(VenueId::Binance, "0.000017");
  EXPECT_TRUE(q.has_value());
  EXPECT_EQ(*q, 10LL);
}

XARB_TEST(DifferentialFlatVsMapOrderBook) {
  FlatOrderBook flat(40);
  MapOrderBook ref(40);
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<PriceTicks> bid_px(6495000, 6499900);
  std::uniform_int_distribution<PriceTicks> ask_px(6500100, 6505000);
  std::uniform_int_distribution<QtyLots> qty_d(0, 500000);

  for (int i = 0; i < 15000; ++i) {
    if (i % 500 == 0) {
      std::vector<PriceLevel> bids, asks;
      for (int k = 0; k < 25; ++k) {
        bids.push_back({bid_px(rng), qty_d(rng) + 100});
        asks.push_back({ask_px(rng), qty_d(rng) + 100});
      }
      flat.apply_snapshot(bids, asks);
      ref.apply_snapshot(bids, asks);
    } else {
      const bool is_bid = (rng() % 2 == 0);
      const PriceTicks p = is_bid ? bid_px(rng) : ask_px(rng);
      const QtyLots q = (rng() % 4 == 0) ? 0 : qty_d(rng);
      flat.apply_delta(is_bid ? Side::Bid : Side::Ask, p, q);
      ref.apply_delta(is_bid ? Side::Bid : Side::Ask, p, q);
    }

    EXPECT_EQ(flat.bid_levels(), ref.bid_levels());
    EXPECT_EQ(flat.ask_levels(), ref.ask_levels());
    EXPECT_EQ(flat.is_crossed(), ref.is_crossed());

    auto fb = flat.best_bid();
    auto rb = ref.best_bid();
    EXPECT_EQ(fb.has_value(), rb.has_value());
    if (fb && rb) {
      EXPECT_EQ(fb->price, rb->price);
      EXPECT_EQ(fb->qty, rb->qty);
    }
    auto fa = flat.best_ask();
    auto ra = ref.best_ask();
    EXPECT_EQ(fa.has_value(), ra.has_value());
    if (fa && ra) {
      EXPECT_EQ(fa->price, ra->price);
      EXPECT_EQ(fa->qty, ra->qty);
    }

    if (i % 200 == 0) {
      auto fw = flat.walk_asks(400000);
      auto rw = ref.walk_asks(400000);
      EXPECT_EQ(fw.filled_lots, rw.filled_lots);
      EXPECT_EQ(fw.worst_price, rw.worst_price);
      EXPECT_NEAR(fw.vwap_ticks, rw.vwap_ticks, 1e-6);
    }
  }
}

XARB_TEST(DepthAwareExecutableCrossStopsAtUnprofitableLevel) {
  FlatOrderBook buy_book;
  FlatOrderBook sell_book;
  // Buy asks: 0.10 BTC @ 100.00, 1.00 BTC @ 101.00
  buy_book.apply_snapshot({}, {{10000, 100000}, {10100, 1000000}});
  // Sell bids: 0.50 BTC @ 100.50, 1.00 BTC @ 99.80
  sell_book.apply_snapshot({{10050, 500000}, {9980, 1000000}}, {});

  // 10 bps buy fee + 10 bps sell fee = ~20 bps round-trip.
  // Level 1 cross: 100.00 vs 100.50 (+50 bps gross -> ~+30 bps net) -> profitable for 0.10 BTC!
  // Level 2 ask is 101.00 > 100.50 -> unprofitable, must STOP at 0.10 BTC (100000 lots)!
  auto res = compute_executable_cross(buy_book, sell_book, 10.0, 10.0, 2.0, 500000);
  EXPECT_EQ(res.executable_lots, 100000LL);
  EXPECT_NEAR(res.buy_vwap_ticks, 10000.0, 1e-6);
  EXPECT_NEAR(res.sell_vwap_ticks, 10050.0, 1e-6);
  EXPECT_EQ(res.buy_levels_used, 1U);
  EXPECT_EQ(res.sell_levels_used, 1U);
  EXPECT_GT(res.net_vwap_edge_bps, 25.0);
}

int main() { return xarb::test::run_all("test_order_book"); }
