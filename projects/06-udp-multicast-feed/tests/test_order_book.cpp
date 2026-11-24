#include "mdf/order_book.hpp"
#include "test_framework.hpp"

using namespace mdf;

MDF_TEST(OrderBookLifecycleAndLevels) {
  ChannelMarketState state(1);
  state.apply_packet(1, std::vector<MarketEvent>{
                            {EventType::kAddOrder, 1001, 1, Side::kBid, 1000, 100},
                            {EventType::kAddOrder, 1001, 2, Side::kBid, 1005, 50},
                            {EventType::kAddOrder, 1001, 3, Side::kBid, 1005, 75},
                            {EventType::kAddOrder, 1001, 4, Side::kAsk, 1010, 80},
                        });

  const auto* book = state.find_instrument(1001);
  EXPECT_TRUE(book != nullptr);
  EXPECT_EQ(book->order_count(), 4U);
  EXPECT_EQ(book->best_bid()->price_ticks, 1005);
  EXPECT_EQ(book->best_bid()->total_qty, 125U);
  EXPECT_EQ(book->best_bid()->order_count, 2U);
  EXPECT_EQ(book->best_ask()->price_ticks, 1010);
  EXPECT_EQ(book->best_ask()->total_qty, 80U);

  // Modify order 2 to a better bid price.
  state.apply_packet(2, std::vector<MarketEvent>{
                            {EventType::kModifyOrder, 1001, 2, Side::kBid, 1008, 60},
                        });
  EXPECT_EQ(book->best_bid()->price_ticks, 1008);
  EXPECT_EQ(book->best_bid()->total_qty, 60U);

  // Partial trade against order 2, then full fill.
  state.apply_packet(3, std::vector<MarketEvent>{
                            {EventType::kTrade, 1001, 2, Side::kBid, 1008, 20},
                        });
  EXPECT_EQ(book->best_bid()->total_qty, 40U);
  EXPECT_EQ(book->stats().trade_count, 1U);
  EXPECT_EQ(book->stats().total_volume, 20U);

  state.apply_packet(4, std::vector<MarketEvent>{
                            {EventType::kTrade, 1001, 2, Side::kBid, 1008, 40},
                        });
  EXPECT_EQ(book->best_bid()->price_ticks, 1005);
  EXPECT_EQ(book->order_count(), 3U);
  EXPECT_EQ(book->stats().total_volume, 60U);

  // Partial and full cancel.
  state.apply_packet(5, std::vector<MarketEvent>{
                            {EventType::kCancelOrder, 1001, 3, Side::kBid, 0, 25},
                            {EventType::kCancelOrder, 1001, 4, Side::kAsk, 0, 80},
                        });
  EXPECT_EQ(book->best_bid()->total_qty, 50U);
  EXPECT_FALSE(book->best_ask().has_value());
}

MDF_TEST(SnapshotRoundTripAndOrderIndependentHash) {
  ChannelMarketState a(2);
  a.apply_packet(1, std::vector<MarketEvent>{
                        {EventType::kAddOrder, 2001, 10, Side::kBid, 500, 100},
                        {EventType::kAddOrder, 2001, 20, Side::kBid, 500, 200},
                        {EventType::kAddOrder, 2002, 30, Side::kAsk, 700, 50},
                        {EventType::kTrade, 2001, 10, Side::kBid, 500, 40},
                    });

  ChannelMarketState b(2);
  b.apply_packet(1, std::vector<MarketEvent>{
                        {EventType::kAddOrder, 2002, 30, Side::kAsk, 700, 50},
                        {EventType::kAddOrder, 2001, 20, Side::kBid, 500, 200},
                        {EventType::kAddOrder, 2001, 10, Side::kBid, 500, 100},
                        {EventType::kTrade, 2001, 10, Side::kBid, 500, 40},
                    });

  EXPECT_TRUE(a == b);
  EXPECT_EQ(a.state_hash(), b.state_hash());
  EXPECT_TRUE(a.diff(b).empty());

  const SnapshotData snap = a.export_snapshot(7);
  ChannelMarketState c(2);
  c.load_snapshot(snap);

  EXPECT_TRUE(a == c);
  EXPECT_EQ(a.state_hash(), c.state_hash());
  EXPECT_EQ(c.last_applied_seq(), 1ULL);

  // Mutating c must change hash and produce a non-empty diff.
  c.apply_packet(2, std::vector<MarketEvent>{
                        {EventType::kCancelOrder, 2002, 30, Side::kAsk, 0, 50},
                    });
  EXPECT_FALSE(a == c);
  EXPECT_NE(a.state_hash(), c.state_hash());
  EXPECT_FALSE(a.diff(c).empty());
}

MDF_TEST_MAIN()
