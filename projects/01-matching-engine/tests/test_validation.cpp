// Negative-case tests: every rejection path, and proof that rejects change nothing.
#include <limits>

#include "test_util.hpp"

namespace me::test {
namespace {

template <class Book>
class Validation : public ::testing::Test {
 protected:
  // tick 5, lot 10, at most 3 resting orders per book
  Engine<Book> engine{make_config(1, 5, 10, 3)};
};
TYPED_TEST_SUITE(Validation, BookTypes);

TYPED_TEST(Validation, RejectsZeroAndNegativeQuantity) {
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, 0)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, -10)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, sell(1, 100, std::numeric_limits<Qty>::min())), RejectReason::InvalidQuantity);
  EXPECT_EQ(this->engine.book(0).size(), 0u);
}

TYPED_TEST(Validation, RejectsQuantityOffLotOrAboveMax) {
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, 15)), RejectReason::InvalidQuantity);
  const Qty max = this->engine.config().symbols[0].max_qty;
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, max + 10)), RejectReason::InvalidQuantity);
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, std::numeric_limits<Qty>::max())), RejectReason::InvalidQuantity);
  EXPECT_EQ(run(this->engine, buy(1, 100, max))[0].type, ReportType::Accepted);
}

TYPED_TEST(Validation, RejectsInvalidTick) {
  EXPECT_EQ(reject_reason(this->engine, buy(1, 101, 10)), RejectReason::InvalidTick);
  EXPECT_EQ(reject_reason(this->engine, sell(1, 104, 10)), RejectReason::InvalidTick);
  EXPECT_EQ(run(this->engine, buy(1, 105, 10))[0].type, ReportType::Accepted);
}

TYPED_TEST(Validation, RejectsNonPositiveAndOversizedPrice) {
  EXPECT_EQ(reject_reason(this->engine, buy(1, 0, 10)), RejectReason::InvalidPrice);
  EXPECT_EQ(reject_reason(this->engine, buy(1, -100, 10)), RejectReason::InvalidPrice);
  EXPECT_EQ(reject_reason(this->engine, buy(1, std::numeric_limits<Price>::min(), 10)), RejectReason::InvalidPrice);
  EXPECT_EQ(reject_reason(this->engine, buy(1, std::numeric_limits<Price>::max(), 10)), RejectReason::InvalidPrice);
}

TYPED_TEST(Validation, RejectsDuplicateLiveOrderId) {
  run(this->engine, buy(1, 100, 10));
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, 10)), RejectReason::DuplicateOrderId);
  EXPECT_EQ(reject_reason(this->engine, sell(1, 200, 10)), RejectReason::DuplicateOrderId);
  // A duplicate must not trade even when its price crosses.
  EXPECT_EQ(reject_reason(this->engine, sell(1, 100, 10)), RejectReason::DuplicateOrderId);
  EXPECT_EQ(this->engine.top_of_book(0).bid, (LevelView{100, 10, 1}));
  EXPECT_EQ(this->engine.stats().trades, 0u);
}

TYPED_TEST(Validation, OrderIdIsReusableOnceTheOrderIsGone) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, cancel(1));
  EXPECT_EQ(run(this->engine, buy(1, 100, 10))[0].type, ReportType::Accepted);
  run(this->engine, sell(2, 100, 10));  // fills order 1 completely
  EXPECT_EQ(run(this->engine, sell(1, 100, 10))[0].type, ReportType::Accepted);
}

TYPED_TEST(Validation, RejectsUnknownSymbolCommandAndSide) {
  EXPECT_EQ(reject_reason(this->engine, buy(1, 100, 10, 1)), RejectReason::UnknownSymbol);
  EXPECT_EQ(reject_reason(this->engine, cancel(1, 7)), RejectReason::UnknownSymbol);

  Command c = buy(1, 100, 10);
  c.type = static_cast<CommandType>(0);
  EXPECT_EQ(reject_reason(this->engine, c), RejectReason::UnknownCommand);
  c.type = static_cast<CommandType>(5);
  EXPECT_EQ(reject_reason(this->engine, c), RejectReason::UnknownCommand);
  c.type = static_cast<CommandType>(255);
  EXPECT_EQ(reject_reason(this->engine, c), RejectReason::UnknownCommand);

  c = buy(1, 100, 10);
  c.side = static_cast<Side>(2);
  EXPECT_EQ(reject_reason(this->engine, c), RejectReason::InvalidSide);
  EXPECT_EQ(this->engine.book(0).size(), 0u);
}

TYPED_TEST(Validation, RejectsUnknownOrderIdForCancelReduceReplace) {
  EXPECT_EQ(reject_reason(this->engine, cancel(1)), RejectReason::UnknownOrderId);
  EXPECT_EQ(reject_reason(this->engine, reduce(1, 10)), RejectReason::UnknownOrderId);
  EXPECT_EQ(reject_reason(this->engine, replace(1, 2, 100, 10)), RejectReason::UnknownOrderId);
}

TYPED_TEST(Validation, ReduceMustBeALotMultiple) {
  run(this->engine, buy(1, 100, 30));
  EXPECT_EQ(reject_reason(this->engine, reduce(1, 15)), RejectReason::InvalidQuantity);
  EXPECT_EQ(run(this->engine, reduce(1, 20))[0].type, ReportType::Reduced);
}

TYPED_TEST(Validation, ReplaceValidatesTickAndLot) {
  run(this->engine, buy(1, 100, 30));
  EXPECT_EQ(reject_reason(this->engine, replace(1, 2, 101, 30)), RejectReason::InvalidTick);
  EXPECT_EQ(reject_reason(this->engine, replace(1, 2, 100, 35)), RejectReason::InvalidQuantity);
  EXPECT_TRUE(this->engine.book(0).contains(1));
}

TYPED_TEST(Validation, BookFullRejectsBeforeMatchingAndRecoversAfterCancel) {
  run(this->engine, buy(1, 100, 10));
  run(this->engine, buy(2, 100, 10));
  run(this->engine, buy(3, 100, 10));
  // Even a fully marketable order is refused while the book is at its bound.
  EXPECT_EQ(reject_reason(this->engine, sell(4, 100, 10)), RejectReason::BookFull);
  EXPECT_EQ(this->engine.stats().trades, 0u);
  // Replace is still possible: it frees a slot before it takes one.
  EXPECT_EQ(run(this->engine, replace(3, 5, 95, 10))[0].type, ReportType::Replaced);
  run(this->engine, cancel(1));
  EXPECT_EQ(run(this->engine, sell(4, 100, 10))[0].type, ReportType::Accepted);
  std::string why;
  EXPECT_TRUE(this->engine.book(0).check_invariants(&why)) << why;
}

TYPED_TEST(Validation, RejectEchoesCommandFieldsAndConsumesOneSequenceNumber) {
  const auto out = run(this->engine, replace(11, 12, 101, 30));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, ReportType::Rejected);
  EXPECT_EQ(out[0].seq, 1u);
  EXPECT_EQ(out[0].order_id, 11u);
  EXPECT_EQ(out[0].other_id, 12u);
  EXPECT_EQ(out[0].price, 101);
  EXPECT_EQ(out[0].qty, 30);
  EXPECT_EQ(out[0].leaves, 0);
}

TEST(EngineConfigTest, RejectsNonsenseConfiguration) {
  EXPECT_THROW(Engine<PoolBook>(make_config(1, 0, 1)), std::invalid_argument);
  EXPECT_THROW(Engine<PoolBook>(make_config(1, 1, 0)), std::invalid_argument);
  EXPECT_THROW(Engine<PoolBook>(make_config(1, 1, 1, 0)), std::invalid_argument);
  EngineConfig overflow = make_config(1);
  overflow.symbols[0].max_qty = std::numeric_limits<Qty>::max() / 2;
  EXPECT_THROW(Engine<MapBook>{overflow}, std::invalid_argument);
}

TEST(EngineConfigTest, EngineWithNoSymbolsRejectsEverything) {
  Engine<PoolBook> engine{make_config(0)};
  EXPECT_EQ(reject_reason(engine, buy(1, 100, 10)), RejectReason::UnknownSymbol);
}

}  // namespace
}  // namespace me::test
