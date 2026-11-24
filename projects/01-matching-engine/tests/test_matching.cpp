// Scenario tests for matching, cancel, reduce and replace, run against both books.
#include "test_util.hpp"

namespace me::test {
namespace {

template <class Book>
class Matching : public ::testing::Test {
 protected:
  Engine<Book> engine{make_config(2)};

  void expect_healthy() {
    for (SymbolId s = 0; s < engine.symbol_count(); ++s) {
      std::string why;
      EXPECT_TRUE(engine.book(s).check_invariants(&why)) << why;
    }
  }
  using L = std::vector<LevelView>;
  using T = std::vector<TradeSummary>;
};
TYPED_TEST_SUITE(Matching, BookTypes);

TYPED_TEST(Matching, PassiveOrderRestsAndIsAcknowledged) {
  const auto out = run(this->engine, buy(1, 100, 10));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, ReportType::Accepted);
  EXPECT_EQ(out[0].seq, 1u);
  EXPECT_EQ(out[0].order_id, 1u);
  EXPECT_EQ(out[0].side, Side::Buy);
  EXPECT_EQ(out[0].price, 100);
  EXPECT_EQ(out[0].qty, 10);
  EXPECT_EQ(out[0].leaves, 10);
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{100, 10, 1}));
  EXPECT_FALSE(this->engine.top_of_book(0).ask.has_value());
  this->expect_healthy();
}

TYPED_TEST(Matching, EqualPriceBuysFillInArrivalOrder) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 100, 10));
  const auto out = run(this->engine, sell(3, 100, 15));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{1, 100, 10}, {2, 100, 5}}));
  // The first buy is gone; the second keeps its unfilled remainder.
  EXPECT_FALSE(this->engine.book(0).contains(1));
  EXPECT_EQ(this->engine.book(0).find(2), (OrderInfo{Side::Buy, 100, 5}));
  EXPECT_FALSE(this->engine.book(0).contains(3));
  this->expect_healthy();
}

TYPED_TEST(Matching, SellSweepsThreeBidLevelsBestPriceFirstThenFifo) {
  run(this->engine, buy(1, 99, 5));
  run(this->engine, buy(2, 101, 5));
  run(this->engine, buy(3, 100, 5));
  run(this->engine, buy(4, 101, 5));
  run(this->engine, buy(5, 100, 5));
  const auto out = run(this->engine, sell(6, 99, 22));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{2, 101, 5}, {4, 101, 5}, {3, 100, 5}, {5, 100, 5}, {1, 99, 2}}));
  EXPECT_EQ(this->engine.depth(0, Side::Buy, 10), (typename TestFixture::L{{99, 3, 1}}));
  EXPECT_TRUE(this->engine.depth(0, Side::Sell, 10).empty());
  this->expect_healthy();
}

TYPED_TEST(Matching, BuySweepsAskLevelsLowestPriceFirst) {
  run(this->engine, sell(1, 103, 5));
  run(this->engine, sell(2, 101, 5));
  run(this->engine, sell(3, 102, 5));
  const auto out = run(this->engine, buy(4, 102, 100));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{2, 101, 5}, {3, 102, 5}}));
  // The remainder rests at its limit; the 103 ask was not reachable.
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{102, 90, 1}));
  EXPECT_EQ(this->engine.top_of_book(0).ask, (LevelView{103, 5, 1}));
  this->expect_healthy();
}

TYPED_TEST(Matching, TradeExecutesAtRestingOrderPrice) {
  run(this->engine, sell(1, 100, 10));
  const auto out = run(this->engine, buy(2, 105, 10));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1].type, ReportType::Trade);
  EXPECT_EQ(out[1].price, 100);
  EXPECT_EQ(out[1].order_id, 2u);  // taker
  EXPECT_EQ(out[1].other_id, 1u);  // maker
  EXPECT_EQ(out[1].side, Side::Buy);
  EXPECT_EQ(out[1].leaves, 0);
  EXPECT_EQ(out[1].other_leaves, 0);
  EXPECT_EQ(this->engine.book(0).size(), 0u);
}

TYPED_TEST(Matching, TradeReportsCarryRunningLeaves) {
  run(this->engine, sell(1, 100, 4));
  run(this->engine, sell(2, 100, 10));
  const auto out = run(this->engine, buy(3, 100, 9));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1].leaves, 5);        // taker after first fill
  EXPECT_EQ(out[1].other_leaves, 0);  // maker 1 fully filled
  EXPECT_EQ(out[2].leaves, 0);
  EXPECT_EQ(out[2].other_leaves, 5);  // maker 2 partially filled
}

TYPED_TEST(Matching, NonCrossingOrdersDoNotTrade) {
  run(this->engine, buy(1, 99, 10));
  const auto out = run(this->engine, sell(2, 100, 10));
  EXPECT_TRUE(trades(out).empty());
  EXPECT_EQ(this->engine.book(0).size(), 2u);
  this->expect_healthy();
}

TYPED_TEST(Matching, CancelRemovesOnlyTheUnfilledRemainder) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, sell(2, 100, 4));
  const auto out = run(this->engine, cancel(1));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, ReportType::Canceled);
  EXPECT_EQ(out[0].qty, 6);  // 4 were already filled and stay filled
  EXPECT_EQ(out[0].side, Side::Buy);
  EXPECT_EQ(out[0].price, 100);
  EXPECT_EQ(this->engine.book(0).size(), 0u);
  EXPECT_EQ(this->engine.stats().traded_qty, 4);
  EXPECT_EQ(reject_reason(this->engine, cancel(1)), RejectReason::UnknownOrderId);
  this->expect_healthy();
}

TYPED_TEST(Matching, CancelFromHeadMiddleAndTailOfQueue) {
  for (OrderId id = 1; id <= 5; ++id) run(this->engine, buy(id, 100, 10));
  run(this->engine, cancel(3));  // middle
  run(this->engine, cancel(1));  // head
  run(this->engine, cancel(5));  // tail
  this->expect_healthy();
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{100, 20, 2}));
  const auto out = run(this->engine, sell(9, 100, 20));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{2, 100, 10}, {4, 100, 10}}));
  this->expect_healthy();
}

TYPED_TEST(Matching, CancelLastOrderRemovesLevel) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 99, 10));
  run(this->engine, cancel(1));
  EXPECT_EQ(this->engine.depth(0, Side::Buy, 10), (typename TestFixture::L{{99, 10, 1}}));
  this->expect_healthy();
}

TYPED_TEST(Matching, ReduceKeepsTimePriority) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 100, 10));
  const auto red = run(this->engine, reduce(1, 3));
  ASSERT_EQ(red.size(), 1u);
  EXPECT_EQ(red[0].type, ReportType::Reduced);
  EXPECT_EQ(red[0].qty, 7);
  EXPECT_EQ(red[0].leaves, 3);
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{100, 13, 2}));
  // Order 1 is still first in the queue.
  const auto out = run(this->engine, sell(3, 100, 5));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{1, 100, 3}, {2, 100, 2}}));
  this->expect_healthy();
}

TYPED_TEST(Matching, ReduceRejectsIncreaseEqualAndZero) {
  run(this->engine, buy(1, 100, 10));
  EXPECT_EQ(reject_reason(this->engine, reduce(1, 11)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, reduce(1, 10)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, reduce(1, 0)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, reduce(1, -1)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, reduce(2, 1)), RejectReason::UnknownOrderId);
  EXPECT_EQ(this->engine.book(0).find(1), (OrderInfo{Side::Buy, 100, 10}));
}

TYPED_TEST(Matching, ReplaceLosesTimePriorityEvenAtSamePrice) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 100, 10));
  const auto rep = run(this->engine, replace(1, 7, 100, 10));
  ASSERT_EQ(rep.size(), 1u);
  EXPECT_EQ(rep[0].type, ReportType::Replaced);
  EXPECT_EQ(rep[0].order_id, 1u);
  EXPECT_EQ(rep[0].other_id, 7u);
  EXPECT_EQ(rep[0].side, Side::Buy);
  EXPECT_EQ(rep[0].other_leaves, 10);
  EXPECT_FALSE(this->engine.book(0).contains(1));
  const auto out = run(this->engine, sell(3, 100, 15));
  EXPECT_EQ(trades(out), (typename TestFixture::T{{2, 100, 10}, {7, 100, 5}}));
  this->expect_healthy();
}

TYPED_TEST(Matching, ReplaceMayReuseItsOwnId) {
  run(this->engine, sell(1, 105, 10));
  const auto rep = run(this->engine, replace(1, 1, 104, 8));
  ASSERT_EQ(rep.size(), 1u);
  EXPECT_EQ(rep[0].type, ReportType::Replaced);
  EXPECT_EQ(this->engine.book(0).find(1), (OrderInfo{Side::Sell, 104, 8}));
  this->expect_healthy();
}

TYPED_TEST(Matching, ReplaceIntoCrossingPriceTrades) {
  run(this->engine, sell(1, 105, 10));
  run(this->engine, buy(2, 100, 10));
  const auto out = run(this->engine, replace(2, 3, 105, 4));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].type, ReportType::Replaced);
  EXPECT_EQ(out[1].type, ReportType::Trade);
  EXPECT_EQ(out[1].order_id, 3u);
  EXPECT_EQ(out[1].other_id, 1u);
  EXPECT_EQ(out[1].qty, 4);
  EXPECT_EQ(this->engine.book(0).find(1), (OrderInfo{Side::Sell, 105, 6}));
  EXPECT_FALSE(this->engine.book(0).contains(3));
  this->expect_healthy();
}

TYPED_TEST(Matching, RejectedReplaceLeavesOriginalUntouched) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 99, 10));
  EXPECT_EQ(reject_reason(this->engine, replace(1, 2, 101, 10)), RejectReason::DuplicateOrderId);
  EXPECT_EQ(reject_reason(this->engine, replace(1, 3, 101, 0)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, replace(1, 3, 0, 10)), RejectReason::InvalidPrice);
  EXPECT_EQ(reject_reason(this->engine, replace(8, 3, 101, 10)), RejectReason::UnknownOrderId);
  EXPECT_EQ(this->engine.book(0).find(1), (OrderInfo{Side::Buy, 100, 10}));
  EXPECT_FALSE(this->engine.book(0).contains(3));
  this->expect_healthy();
}

TYPED_TEST(Matching, SymbolsHaveIndependentBooks) {
  run(this->engine, buy(1, 100, 10, 0));
  // Same id and a crossing price on another symbol: no duplicate, no trade.
  const auto out = run(this->engine, sell(1, 100, 10, 1));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, ReportType::Accepted);
  EXPECT_EQ(out[0].symbol, 1u);
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{100, 10, 1}));
  EXPECT_EQ(this->engine.top_of_book(1).ask, (LevelView{100, 10, 1}));
  // Cancelling on symbol 1 leaves symbol 0 alone.
  run(this->engine, cancel(1, 1));
  EXPECT_TRUE(this->engine.book(0).contains(1));
  EXPECT_FALSE(this->engine.book(1).contains(1));
}

TYPED_TEST(Matching, SequenceNumbersAreGaplessAcrossSymbolsAndReportKinds) {
  std::vector<Report> all;
  const auto sink = [&all](const Report& r) { all.push_back(r); };
  this->engine.process(buy(1, 100, 10, 0), sink);
  this->engine.process(buy(2, 100, 10, 1), sink);
  this->engine.process(sell(3, 100, 25, 0), sink);  // accept + trade
  this->engine.process(cancel(99, 1), sink);        // reject
  this->engine.process(reduce(2, 5, 1), sink);
  this->engine.process(replace(3, 4, 101, 5, 0), sink);
  this->engine.process(cancel(4, 0), sink);
  ASSERT_EQ(all.size(), 8u);
  for (std::size_t i = 0; i < all.size(); ++i) EXPECT_EQ(all[i].seq, i + 1);
  EXPECT_EQ(this->engine.last_seq(), 8u);
  const EngineStats& s = this->engine.stats();
  EXPECT_EQ(s.commands, 7u);
  EXPECT_EQ(s.accepted, 3u);
  EXPECT_EQ(s.trades, 1u);
  EXPECT_EQ(s.rejected, 1u);
  EXPECT_EQ(s.reduced, 1u);
  EXPECT_EQ(s.replaced, 1u);
  EXPECT_EQ(s.canceled, 1u);
}

TYPED_TEST(Matching, DepthSnapshotIsAggregatedAndBestFirst) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 100, 5));
  run(this->engine, buy(3, 98, 7));
  run(this->engine, buy(4, 99, 1));
  run(this->engine, sell(5, 103, 2));
  run(this->engine, sell(6, 102, 3));
  run(this->engine, sell(7, 102, 4));
  EXPECT_EQ(this->engine.depth(0, Side::Buy, 10), (typename TestFixture::L{{100, 15, 2}, {99, 1, 1}, {98, 7, 1}}));
  EXPECT_EQ(this->engine.depth(0, Side::Sell, 10), (typename TestFixture::L{{102, 7, 2}, {103, 2, 1}}));
  EXPECT_EQ(this->engine.depth(0, Side::Buy, 2), (typename TestFixture::L{{100, 15, 2}, {99, 1, 1}}));
  EXPECT_TRUE(this->engine.depth(0, Side::Buy, 0).empty());
}

TYPED_TEST(Matching, ManyLevelsStaySortedWhenInsertedOutOfOrder) {
  // Exercises level insertion far from the top of book (binary-search path in PoolBook).
  OrderId id = 0;
  for (Price p : {150, 110, 190, 101, 170, 130, 105, 120, 199, 160, 140, 180, 115, 125, 135, 145}) {
    run(this->engine, sell(++id, p, 1));
    run(this->engine, buy(++id, p - 100, 1));
  }
  this->expect_healthy();
  const auto asks = this->engine.depth(0, Side::Sell, 100);
  const auto bids = this->engine.depth(0, Side::Buy, 100);
  ASSERT_EQ(asks.size(), 16u);
  ASSERT_EQ(bids.size(), 16u);
  for (std::size_t i = 1; i < asks.size(); ++i) EXPECT_LT(asks[i - 1].price, asks[i].price);
  for (std::size_t i = 1; i < bids.size(); ++i) EXPECT_GT(bids[i - 1].price, bids[i].price);
  // Cancel a deep level, then sweep everything.
  run(this->engine, cancel(1));  // the 150 ask
  const auto out = run(this->engine, buy(1000, 200, 100));
  EXPECT_EQ(trades(out).size(), 15u);
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{200, 85, 1}));
  this->expect_healthy();
}

}  // namespace
}  // namespace me::test
