#include "hft/adapters/matching_adapter.hpp"
#include "hft/runtime/pipeline.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

HFT_TEST(PoolBookAndMapBookDifferentialEquivalence) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.maker_fee_bps_x100 = -20;
  venue.taker_fee_bps_x100 = 30;

  MatchingEngineAdapter pool_ex(venue, insts, BookBackend::Pool);
  MatchingEngineAdapter map_ex(venue, insts, BookBackend::Map);

  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  for (const auto& env : feed) {
    const auto& md = std::get<MarketDataEvent>(env.payload);
    pool_ex.apply_market_event(0, md, env.event_time_ns);
    map_ex.apply_market_event(0, md, env.event_time_ns);
  }
  HFT_EXPECT_EQ(pool_ex.book_hash(0), map_ex.book_hash(0));
  HFT_EXPECT_TRUE(pool_ex.check_invariants());
  HFT_EXPECT_TRUE(map_ex.check_invariants());

  // Submit identical sequence of GTC, IOC, Market, Post-Only, Cancel, Replace orders to both!
  std::vector<OrderIntent> intents;
  auto make_new = [](OrderId id, const char* pfx, Side s, OrderType ot, TimeInForce tif,
                     bool post_only, std::int64_t px, std::uint64_t qty) {
    OrderIntent oi{};
    oi.action = OrderAction::New;
    oi.client_order_id = id;
    oi.token = ClientToken::from_uint64(pfx, id);
    oi.side = s;
    oi.order_type = ot;
    oi.tif = tif;
    oi.post_only = post_only;
    oi.limit_price = Price{px};
    oi.qty = Quantity{qty};
    return oi;
  };

  const TopOfBook tob = pool_ex.top_of_book(0);
  HFT_EXPECT_TRUE(tob.valid_two_sided());

  // 1. Post-only crossing -> rejected identically on both backends
  intents.push_back(make_new(1, "DIF", Side::Buy, OrderType::Limit, TimeInForce::GTC, true,
                             tob.ask->price.ticks, 20));
  // 2. IOC partial fill + remainder auto-canceled
  intents.push_back(make_new(2, "DIF", Side::Buy, OrderType::Limit, TimeInForce::IOC, false,
                             tob.ask->price.ticks, tob.ask->qty.lots + 25));
  // 3. Resting passive bid
  intents.push_back(make_new(3, "DIF", Side::Buy, OrderType::Limit, TimeInForce::GTC, true,
                             tob.bid->price.ticks, 30));
  // 4. Replace resting passive bid to lower price & size
  OrderIntent rep{};
  rep.action = OrderAction::Replace;
  rep.client_order_id = 4;
  rep.token = ClientToken::from_uint64("DIF", 4);
  rep.orig_token = ClientToken::from_uint64("DIF", 3);
  rep.side = Side::Buy;
  rep.limit_price = Price{tob.bid->price.ticks - 1};
  rep.qty = Quantity{20};
  intents.push_back(rep);
  // 5. Cancel replaced order
  OrderIntent cxl{};
  cxl.action = OrderAction::Cancel;
  cxl.client_order_id = 5;
  cxl.token = ClientToken::from_uint64("CXL", 5);
  cxl.orig_token = rep.token;
  cxl.side = Side::Buy;
  intents.push_back(cxl);
  // 6. Market sell sweeping bids
  intents.push_back(make_new(6, "DIF", Side::Sell, OrderType::Market, TimeInForce::IOC, false, 0, 15));

  SimTimeNs t{5'000'000'000LL};
  for (const auto& oi : intents) {
    t = t + 10'000LL;
    std::vector<ExecutionReport> r_pool;
    std::vector<ExecutionReport> r_map;
    HFT_EXPECT_EQ(pool_ex.submit_order(0, oi, t, r_pool), ErrorCode::Ok);
    HFT_EXPECT_EQ(map_ex.submit_order(0, oi, t, r_map), ErrorCode::Ok);
    HFT_EXPECT_EQ(r_pool.size(), r_map.size());
    for (std::size_t i = 0; i < r_pool.size(); ++i) {
      HFT_EXPECT_TRUE(r_pool[i] == r_map[i]);
    }
  }

  HFT_EXPECT_EQ(pool_ex.book_hash(0), map_ex.book_hash(0));
  HFT_EXPECT_TRUE(pool_ex.check_invariants());
  HFT_EXPECT_TRUE(map_ex.check_invariants());
}

HFT_TEST(OuchMatchingBridgeExecutesOnProject01PoolBook) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  OuchMatchingBridge bridge(insts);

  ouch::sim::AddResult r1{};
  bridge.add(101, ouch::Symbol("AAPL"), false, 1'000'100U, 100, false, r1);
  HFT_EXPECT_EQ(r1.resting, 100U);
  HFT_EXPECT_EQ(bridge.open_shares(101), 100U);

  // IOC buy sweeps 60, cancels 0; resting ask has 40 left.
  ouch::sim::AddResult r2{};
  bridge.add(102, ouch::Symbol("AAPL"), true, 1'000'100U, 60, true, r2);
  HFT_EXPECT_EQ(r2.fills.size(), 1U);
  HFT_EXPECT_EQ(r2.fills[0].shares, 60U);
  HFT_EXPECT_EQ(r2.fills[0].resting_ref, 101ULL);
  HFT_EXPECT_EQ(bridge.open_shares(101), 40U);

  // Partial reduce to 15 shares -> removes 25.
  HFT_EXPECT_EQ(bridge.reduce(101, 15), 25U);
  HFT_EXPECT_EQ(bridge.open_shares(101), 15U);

  // Full cancel -> removes 15.
  HFT_EXPECT_EQ(bridge.reduce(101, 0), 15U);
  HFT_EXPECT_EQ(bridge.open_shares(101), 0U);
}

int main() { return hft::test::run_all("test_matching_differential"); }
