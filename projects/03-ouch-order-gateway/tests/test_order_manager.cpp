#include <gtest/gtest.h>

#include <vector>

#include "ouch/order_manager.hpp"

using namespace ouch;
using namespace ouch::wire;

namespace {

const Symbol kSym("AAPL");
constexpr std::uint32_t kPx = 1'500'000;

EnterOrder enter(const char* token, char side, std::uint32_t shares, std::uint32_t price = kPx) {
  EnterOrder e;
  e.token = Token(token);
  e.side = side;
  e.shares = shares;
  e.stock = kSym;
  e.price = price;
  return e;
}

Accepted accepted(const EnterOrder& e, std::uint64_t ref) {
  Accepted a;
  a.token = e.token;
  a.side = e.side;
  a.shares = e.shares;
  a.stock = e.stock;
  a.price = e.price;
  a.order_ref = ref;
  return a;
}

Executed executed(const char* token, std::uint32_t shares, std::uint64_t match,
                  std::uint32_t price = kPx) {
  Executed x;
  x.token = Token(token);
  x.shares = shares;
  x.price = price;
  x.match_number = match;
  return x;
}

Canceled canceled(const char* token, std::uint32_t dec, char reason = 'U') {
  Canceled c;
  c.token = Token(token);
  c.decrement_shares = dec;
  c.reason = reason;
  return c;
}

Replaced replaced(const char* token, const char* prev, std::uint32_t shares, std::uint32_t price,
                  std::uint64_t ref) {
  Replaced r;
  r.token = Token(token);
  r.previous_token = Token(prev);
  r.shares = shares;
  r.price = price;
  r.order_ref = ref;
  return r;
}

struct Oms : ::testing::Test {
  OrderManager oms;
  std::vector<OrderEvent> events;

  void SetUp() override {
    oms.set_listener([this](const OrderUpdate& u) { events.push_back(u.event); });
  }
  void TearDown() override { EXPECT_EQ(oms.check_invariants(), ""); }

  const Order& order(const char* token) {
    const Order* o = oms.find(Token(token));
    EXPECT_NE(o, nullptr);
    return *o;
  }
  // Enters and acknowledges an order.
  void live(const char* token, char side, std::uint32_t shares, std::uint64_t ref) {
    const auto e = enter(token, side, shares);
    ASSERT_TRUE(oms.on_enter_sent(e, 1));
    oms.on_exchange(accepted(e, ref), 2);
  }
};

}  // namespace

TEST_F(Oms, EnterAcceptFill) {
  const auto e = enter("A", 'B', 100);
  ASSERT_TRUE(oms.on_enter_sent(e, 10));
  EXPECT_EQ(order("A").state, OrderState::PendingNew);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 100u);
  EXPECT_EQ(oms.unresolved().size(), 1u);

  oms.on_exchange(accepted(e, 77), 25);
  EXPECT_EQ(order("A").state, OrderState::Live);
  EXPECT_EQ(order("A").order_ref, 77u);
  EXPECT_EQ(order("A").sent_ns, 10u);
  EXPECT_EQ(order("A").acked_ns, 25u);
  EXPECT_TRUE(oms.unresolved().empty());

  oms.on_exchange(executed("A", 100, 1), 30);
  EXPECT_EQ(order("A").state, OrderState::Filled);
  EXPECT_EQ(order("A").cum_qty, 100u);
  EXPECT_EQ(order("A").leaves, 0u);
  EXPECT_EQ(order("A").notional, 100ull * kPx);
  EXPECT_EQ(oms.exposure(kSym).position, 100);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
  EXPECT_EQ(events, (std::vector<OrderEvent>{OrderEvent::Submitted, OrderEvent::Accepted,
                                             OrderEvent::Executed}));
}

TEST_F(Oms, DuplicateLocalTokenIsRefused) {
  ASSERT_TRUE(oms.on_enter_sent(enter("A", 'B', 100), 1));
  EXPECT_FALSE(oms.on_enter_sent(enter("A", 'B', 50), 2));
  EXPECT_EQ(oms.orders().size(), 1u);
  EXPECT_EQ(order("A").qty, 100u);
}

TEST_F(Oms, PartialFillsStayLiveAndAccountingIsConsistent) {
  live("A", 'S', 300, 1);
  oms.on_exchange(executed("A", 100, 1, 1'500'000), 3);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").cum_qty, 100u);
  EXPECT_EQ(order("A").leaves, 200u);
  EXPECT_EQ(order("A").qty, 300u);
  EXPECT_EQ(oms.exposure(kSym).position, -100);
  EXPECT_EQ(oms.exposure(kSym).open_sell, 200u);

  oms.on_exchange(executed("A", 50, 2, 1'501'000), 4);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").cum_qty, 150u);
  EXPECT_EQ(order("A").leaves, 150u);
  EXPECT_EQ(order("A").notional, 100ull * 1'500'000 + 50ull * 1'501'000);

  oms.on_exchange(executed("A", 150, 3), 5);
  EXPECT_EQ(order("A").state, OrderState::Filled);
  EXPECT_EQ(oms.exposure(kSym).position, -300);
  EXPECT_EQ(oms.exposure(kSym).open_sell, 0u);
}

TEST_F(Oms, RejectReleasesExposure) {
  ASSERT_TRUE(oms.on_enter_sent(enter("A", 'B', 100), 1));
  oms.on_exchange(Rejected{0, Token("A"), 'S'}, 2);
  EXPECT_EQ(order("A").state, OrderState::Rejected);
  EXPECT_EQ(order("A").reason, 'S');
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
  EXPECT_TRUE(oms.unresolved().empty());
  // A second copy changes nothing.
  oms.on_exchange(Rejected{0, Token("A"), 'S'}, 3);
  EXPECT_EQ(oms.stats().duplicates, 1u);
}

TEST_F(Oms, FullCancel) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 3));
  EXPECT_EQ(order("A").pending, PendingRequest::Cancel);
  EXPECT_EQ(oms.unresolved().size(), 1u);
  oms.on_exchange(canceled("A", 100), 4);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").pending, PendingRequest::None);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
}

TEST_F(Oms, PartialCancelReducesButStaysLive) {
  live("A", 'B', 100, 1);
  oms.on_exchange(executed("A", 20, 1), 3);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 30}, 4));
  oms.on_exchange(canceled("A", 50), 5);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").leaves, 30u);
  EXPECT_EQ(order("A").qty, 50u);
  EXPECT_EQ(order("A").pending, PendingRequest::None);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 30u);
}

TEST_F(Oms, CancelRacingFullFillEndsFilled) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 3));
  // The fill won the race; the exchange silently ignores the cancel.
  oms.on_exchange(executed("A", 100, 1), 4);
  EXPECT_EQ(order("A").state, OrderState::Filled);
  EXPECT_EQ(order("A").pending, PendingRequest::None);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, CancelRacingPartialFillEndsCanceledWithFill) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 3));
  oms.on_exchange(executed("A", 40, 1), 4);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").pending, PendingRequest::Cancel);
  oms.on_exchange(canceled("A", 60), 5);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").cum_qty, 40u);
  EXPECT_EQ(order("A").leaves, 0u);
  EXPECT_EQ(oms.exposure(kSym).position, 40);
}

TEST_F(Oms, PartialCancelSatisfiedByFillsClearsPending) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 50}, 3));
  // 60 shares trade first, leaving 40 <= 50: the exchange has nothing to cancel.
  oms.on_exchange(executed("A", 60, 1), 4);
  EXPECT_EQ(order("A").pending, PendingRequest::None);
  EXPECT_EQ(order("A").leaves, 40u);
}

TEST_F(Oms, CancelRequestNeedsALiveOrder) {
  EXPECT_FALSE(oms.on_cancel_sent(CancelOrder{Token("NOPE"), 0}, 1));
  live("A", 'B', 100, 1);
  oms.on_exchange(executed("A", 100, 1), 3);
  EXPECT_FALSE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 4));
}

TEST_F(Oms, DuplicateAcceptedAndExecutionDoNotDoubleCount) {
  const auto e = enter("A", 'B', 100);
  ASSERT_TRUE(oms.on_enter_sent(e, 1));
  oms.on_exchange(accepted(e, 9), 2);
  oms.on_exchange(accepted(e, 9), 3);
  oms.on_exchange(executed("A", 40, 501), 4);
  oms.on_exchange(executed("A", 40, 501), 5);
  oms.on_exchange(executed("A", 40, 501), 6);
  EXPECT_EQ(order("A").cum_qty, 40u);
  EXPECT_EQ(order("A").leaves, 60u);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(oms.exposure(kSym).position, 40);
  EXPECT_EQ(oms.stats().duplicates, 3u);

  // A different match number is a different fill, even at the same size.
  oms.on_exchange(executed("A", 40, 502), 7);
  EXPECT_EQ(order("A").cum_qty, 80u);
}

TEST_F(Oms, SameMatchNumberOnBothSidesOfASelfTradeCountsOncePerOrder) {
  live("BUY", 'B', 100, 1);
  live("SELL", 'S', 100, 2);
  oms.on_exchange(executed("BUY", 100, 7), 3);
  oms.on_exchange(executed("SELL", 100, 7), 4);
  EXPECT_EQ(order("BUY").state, OrderState::Filled);
  EXPECT_EQ(order("SELL").state, OrderState::Filled);
  EXPECT_EQ(oms.exposure(kSym).position, 0);
  EXPECT_EQ(oms.stats().duplicates, 0u);
}

TEST_F(Oms, DuplicateCanceledIsIgnored) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 3));
  oms.on_exchange(canceled("A", 100), 4);
  oms.on_exchange(canceled("A", 100), 5);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(oms.stats().duplicates, 1u);
  EXPECT_EQ(oms.stats().anomalies, 0u);
}

TEST_F(Oms, DuplicatePartialCancelIsIgnored) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("A"), 40}, 3));
  oms.on_exchange(canceled("A", 60), 4);
  oms.on_exchange(canceled("A", 60), 5);  // would wrongly take leaves to zero
  EXPECT_EQ(order("A").leaves, 40u);
  EXPECT_EQ(order("A").state, OrderState::Live);
  EXPECT_EQ(oms.stats().duplicates, 1u);
}

TEST_F(Oms, UnsolicitedCancelIsApplied) {
  live("A", 'B', 100, 1);
  oms.on_exchange(canceled("A", 100, 'S'), 3);  // supervisory
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").reason, 'S');
}

TEST_F(Oms, ExecutionBeforeAcceptedIsAnImplicitAccept) {
  const auto e = enter("A", 'B', 100);
  ASSERT_TRUE(oms.on_enter_sent(e, 1));
  oms.on_exchange(executed("A", 30, 1), 2);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").cum_qty, 30u);
  EXPECT_EQ(order("A").leaves, 70u);
  EXPECT_EQ(oms.stats().implicit_accepts, 1u);

  // The late Accepted only supplies the reference number.
  oms.on_exchange(accepted(e, 55), 3);
  EXPECT_EQ(order("A").order_ref, 55u);
  EXPECT_EQ(order("A").cum_qty, 30u);
  EXPECT_EQ(order("A").leaves, 70u);
  EXPECT_EQ(order("A").state, OrderState::PartiallyFilled);
  EXPECT_EQ(oms.stats().duplicates, 0u);
}

TEST_F(Oms, CancelBeforeAcceptedForIocOrder) {
  const auto e = enter("A", 'B', 100);
  ASSERT_TRUE(oms.on_enter_sent(e, 1));
  oms.on_exchange(canceled("A", 100, 'I'), 2);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  oms.on_exchange(accepted(e, 5), 3);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").order_ref, 5u);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
}

TEST_F(Oms, AcceptedDeadOrderIsTerminal) {
  const auto e = enter("A", 'B', 100);
  ASSERT_TRUE(oms.on_enter_sent(e, 1));
  auto a = accepted(e, 5);
  a.order_state = 'D';
  oms.on_exchange(a, 2);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").leaves, 0u);
}

TEST_F(Oms, UnknownTokensAreCountedNotApplied) {
  oms.on_exchange(executed("GHOST", 10, 1), 1);
  oms.on_exchange(canceled("GHOST", 10), 2);
  oms.on_exchange(accepted(enter("GHOST", 'B', 10), 1), 3);
  oms.on_exchange(Rejected{0, Token("GHOST"), 'O'}, 4);
  oms.on_exchange(replaced("GHOST2", "GHOST", 10, kPx, 2), 5);
  EXPECT_EQ(oms.stats().unknown_tokens, 5u);
  EXPECT_TRUE(oms.orders().empty());
  EXPECT_EQ(oms.exposure(kSym), SymbolExposure{});
}

TEST_F(Oms, OverfillIsCountedButExchangeTruthWins) {
  live("A", 'B', 100, 1);
  oms.on_exchange(executed("A", 130, 1), 3);
  EXPECT_EQ(order("A").state, OrderState::Filled);
  EXPECT_EQ(order("A").cum_qty, 130u);
  EXPECT_EQ(order("A").leaves, 0u);
  EXPECT_EQ(oms.exposure(kSym).position, 130);
  EXPECT_EQ(oms.stats().anomalies, 1u);
}

TEST_F(Oms, FillOnDeadOrderStillMovesPosition) {
  live("A", 'S', 100, 1);
  oms.on_exchange(canceled("A", 100, 'S'), 3);
  oms.on_exchange(executed("A", 10, 9), 4);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("A").cum_qty, 10u);
  EXPECT_EQ(oms.exposure(kSym).position, -10);
  EXPECT_EQ(oms.stats().anomalies, 1u);
}

// ------------------------------------------------------------------ replace

TEST_F(Oms, ReplaceSuccess) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 150, 1'490'000, kTifMarketHours, 'Y', 'N', 0}, 3));
  EXPECT_EQ(order("A").pending, PendingRequest::Replace);
  EXPECT_EQ(order("B").state, OrderState::PendingNew);
  EXPECT_EQ(order("B").replaces, Token("A"));
  // Both count as exposure until the exchange confirms the swap.
  EXPECT_EQ(oms.exposure(kSym).open_buy, 250u);

  oms.on_exchange(replaced("B", "A", 150, 1'490'000, 2), 4);
  EXPECT_EQ(order("A").state, OrderState::Replaced);
  EXPECT_EQ(order("A").replaced_by, Token("B"));
  EXPECT_EQ(order("A").leaves, 0u);
  EXPECT_EQ(order("B").state, OrderState::Live);
  EXPECT_EQ(order("B").leaves, 150u);
  EXPECT_EQ(order("B").req.price, 1'490'000u);
  EXPECT_EQ(order("B").order_ref, 2u);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 150u);
  EXPECT_TRUE(oms.unresolved().empty());

  oms.on_exchange(replaced("B", "A", 150, 1'490'000, 2), 5);
  EXPECT_EQ(oms.stats().duplicates, 1u);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 150u);
}

TEST_F(Oms, ReplaceAfterPartialFillUsesExchangeOutstanding) {
  live("A", 'B', 100, 1);
  oms.on_exchange(executed("A", 30, 1), 3);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 200, kPx, kTifMarketHours, 'Y', 'N', 0}, 4));
  EXPECT_EQ(order("B").leaves, 170u);  // 200 total - 30 already executed
  // 20 more trade on the old order before the exchange processes the replace.
  oms.on_exchange(executed("A", 20, 2), 5);
  oms.on_exchange(replaced("B", "A", 150, kPx, 2), 6);
  EXPECT_EQ(order("A").state, OrderState::Replaced);
  EXPECT_EQ(order("A").cum_qty, 50u);
  EXPECT_EQ(order("B").leaves, 150u);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 150u);
  EXPECT_EQ(oms.exposure(kSym).position, 50);
}

TEST_F(Oms, ReplaceRejectedLeavesOriginalWorking) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 150, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  oms.on_exchange(Rejected{0, Token("B"), 'X'}, 4);
  EXPECT_EQ(order("B").state, OrderState::Rejected);
  EXPECT_EQ(order("A").state, OrderState::Live);
  EXPECT_EQ(order("A").pending, PendingRequest::None);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 100u);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, ReplaceLosesRaceToFill) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 150, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  oms.on_exchange(executed("A", 100, 1), 4);
  EXPECT_EQ(order("A").state, OrderState::Filled);
  // No response will ever come for B: it is terminated locally.
  EXPECT_EQ(order("B").state, OrderState::Rejected);
  EXPECT_EQ(order("B").reason, kReasonLocalAbort);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, LocallyAbortedReplacementIsRevivedIfExchangeConfirmsIt) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 150, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  oms.on_exchange(canceled("A", 100, 'S'), 4);
  EXPECT_EQ(order("B").state, OrderState::Rejected);
  oms.on_exchange(replaced("B", "A", 150, kPx, 2), 5);
  EXPECT_EQ(order("B").state, OrderState::Live);
  EXPECT_EQ(order("B").leaves, 150u);
  EXPECT_EQ(oms.stats().revived, 1u);
}

TEST_F(Oms, ExecutionOnReplacementBeforeReplacedMessage) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 100, 1'510'000, kTifMarketHours, 'Y', 'N', 0}, 3));
  oms.on_exchange(executed("B", 40, 1), 4);  // overtook the Replaced message
  EXPECT_EQ(order("B").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("A").pending, PendingRequest::Replace);
  oms.on_exchange(replaced("B", "A", 100, 1'510'000, 2), 5);
  EXPECT_EQ(order("A").state, OrderState::Replaced);
  EXPECT_EQ(order("B").state, OrderState::PartiallyFilled);
  EXPECT_EQ(order("B").cum_qty, 40u);
  EXPECT_EQ(order("B").leaves, 60u);
  EXPECT_EQ(order("B").order_ref, 2u);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 60u);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, CancelOnReplacementBeforeReplacedMessageIsNotUndone) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 400, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("B"), 200}, 4));
  // The cancel's response overtakes the Replaced message.
  oms.on_exchange(canceled("B", 200), 5);
  EXPECT_EQ(order("B").leaves, 200u);
  oms.on_exchange(replaced("B", "A", 400, kPx, 2), 6);  // reports the pre-cancel size
  EXPECT_EQ(order("B").leaves, 200u);
  EXPECT_EQ(order("B").qty, 200u);
  EXPECT_EQ(order("A").state, OrderState::Replaced);
  oms.on_exchange(executed("B", 200, 1), 7);
  EXPECT_EQ(order("B").state, OrderState::Filled);
  EXPECT_EQ(oms.exposure(kSym).open_buy, 0u);
  EXPECT_EQ(oms.stats().anomalies, 0u);
}

TEST_F(Oms, LateReplacedCorrectsOnlyTheEstimationError) {
  live("A", 'B', 100, 1);
  oms.on_exchange(executed("A", 30, 1), 3);
  // Estimate for the replacement: 200 total - 30 executed = 170 open.
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 200, kPx, kTifMarketHours, 'Y', 'N', 0}, 4));
  oms.on_exchange(executed("A", 20, 2), 5);  // 20 more trade on the old order first
  oms.on_exchange(executed("B", 50, 3), 6);  // overtakes the Replaced: 170 - 50 = 120
  EXPECT_EQ(order("B").leaves, 120u);
  // The exchange started the replacement at 150 (200 - 50 executed), 20 less than assumed.
  oms.on_exchange(replaced("B", "A", 150, kPx, 2), 7);
  EXPECT_EQ(order("B").leaves, 100u);
  EXPECT_EQ(order("B").cum_qty, 50u);
  EXPECT_EQ(order("B").state, OrderState::PartiallyFilled);
}

TEST_F(Oms, CancelMadeMootByReplacementSizeStopsWaiting) {
  live("A", 'B', 500, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 500, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  // Cancel the (unconfirmed) replacement down to 250, believing it has 500 open.
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("B"), 250}, 4));
  // 400 traded on the old order first, so the replacement starts with 100:
  // already under the target, and the exchange will not answer the cancel.
  oms.on_exchange(executed("A", 400, 1), 5);
  oms.on_exchange(replaced("B", "A", 100, kPx, 2), 6);
  EXPECT_EQ(order("B").state, OrderState::Live);
  EXPECT_EQ(order("B").leaves, 100u);
  EXPECT_EQ(order("B").pending, PendingRequest::None);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, ReplaceRequestPreconditions) {
  const ReplaceOrder r{Token("A"), Token("B"), 150, kPx, kTifMarketHours, 'Y', 'N', 0};
  EXPECT_FALSE(oms.on_replace_sent(r, 1));  // unknown order
  ASSERT_TRUE(oms.on_enter_sent(enter("A", 'B', 100), 1));
  EXPECT_FALSE(oms.on_replace_sent(r, 2));  // not acknowledged yet
  oms.on_exchange(accepted(enter("A", 'B', 100), 1), 3);
  ASSERT_TRUE(oms.on_replace_sent(r, 4));
  // One request at a time.
  EXPECT_FALSE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("C"), 150, kPx, kTifMarketHours, 'Y', 'N', 0}, 5));
  EXPECT_FALSE(oms.on_cancel_sent(CancelOrder{Token("A"), 0}, 6));
  EXPECT_EQ(oms.orders().size(), 2u);
}

// ------------------------------------------------------------- disconnects

TEST_F(Oms, DisconnectMakesInFlightRequestsUncertain) {
  live("LIVE", 'B', 100, 1);
  live("CXL", 'B', 100, 2);
  ASSERT_TRUE(oms.on_enter_sent(enter("NEW", 'B', 100), 3));
  ASSERT_TRUE(oms.on_cancel_sent(CancelOrder{Token("CXL"), 0}, 4));

  oms.on_disconnect();
  EXPECT_EQ(order("NEW").state, OrderState::Reconciling);
  EXPECT_EQ(order("LIVE").state, OrderState::Live);  // acknowledged orders are not in doubt
  EXPECT_EQ(order("CXL").state, OrderState::Live);
  EXPECT_TRUE(order("CXL").pending_uncertain);
  EXPECT_EQ(oms.stats().uncertain, 2u);
  // An uncertain order still counts as exposure: it may be live.
  EXPECT_EQ(oms.exposure(kSym).open_buy, 300u);

  // A second disconnect before resolution does not recount.
  oms.on_disconnect();
  EXPECT_EQ(oms.stats().uncertain, 2u);
}

TEST_F(Oms, ReconcilingResolvesToAcceptedRejectedOrFilled) {
  for (const char* t : {"A", "B", "C"}) ASSERT_TRUE(oms.on_enter_sent(enter(t, 'B', 100), 1));
  oms.on_disconnect();
  oms.on_resent(Token("A"));
  EXPECT_EQ(order("A").state, OrderState::Reconciling);  // resend alone proves nothing
  EXPECT_EQ(order("A").resends, 1u);

  oms.on_exchange(accepted(enter("A", 'B', 100), 1), 5);
  oms.on_exchange(Rejected{0, Token("B"), 'H'}, 6);
  oms.on_exchange(executed("C", 100, 1), 7);
  EXPECT_EQ(order("A").state, OrderState::Live);
  EXPECT_EQ(order("B").state, OrderState::Rejected);
  EXPECT_EQ(order("C").state, OrderState::Filled);
  EXPECT_TRUE(oms.unresolved().empty());
}

TEST_F(Oms, UncertainReplaceResolvesThroughReplay) {
  live("A", 'B', 100, 1);
  ASSERT_TRUE(oms.on_replace_sent(
      ReplaceOrder{Token("A"), Token("B"), 150, kPx, kTifMarketHours, 'Y', 'N', 0}, 3));
  oms.on_disconnect();
  EXPECT_EQ(order("B").state, OrderState::Reconciling);
  EXPECT_TRUE(order("A").pending_uncertain);
  oms.on_resent(Token("B"));
  EXPECT_FALSE(order("A").pending_uncertain);
  oms.on_exchange(replaced("B", "A", 150, kPx, 2), 5);
  EXPECT_EQ(order("B").state, OrderState::Live);
  EXPECT_EQ(order("A").state, OrderState::Replaced);
}

TEST_F(Oms, ReplayingAnEntireStreamTwiceChangesNothing) {
  const auto a = enter("A", 'B', 100);
  const auto b = enter("B", 'S', 100);
  ASSERT_TRUE(oms.on_enter_sent(a, 1));
  ASSERT_TRUE(oms.on_enter_sent(b, 1));
  const std::vector<OutboundMessage> stream = {
      accepted(a, 1),       accepted(b, 2),         executed("A", 60, 1),
      executed("B", 60, 1), canceled("A", 40, 'S'), executed("B", 40, 2),
  };
  for (const auto& m : stream) oms.on_exchange(m, 2);
  const SymbolExposure before = oms.exposure(kSym);
  const auto a_cum = order("A").cum_qty;
  const auto b_cum = order("B").cum_qty;

  for (const auto& m : stream) oms.on_exchange(m, 3);
  EXPECT_EQ(oms.exposure(kSym), before);
  EXPECT_EQ(order("A").cum_qty, a_cum);
  EXPECT_EQ(order("B").cum_qty, b_cum);
  EXPECT_EQ(order("A").state, OrderState::Canceled);
  EXPECT_EQ(order("B").state, OrderState::Filled);
  EXPECT_EQ(oms.stats().duplicates, stream.size());
}
