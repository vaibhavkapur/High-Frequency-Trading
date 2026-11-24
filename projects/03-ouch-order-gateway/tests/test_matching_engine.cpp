#include <gtest/gtest.h>

#include "ouch/sim/matching_engine.hpp"

using namespace ouch;
using namespace ouch::sim;

namespace {
const Symbol kSym("AAPL");
}

TEST(Matching, RestsWhenNothingCrosses) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, true, 100, 50, false, r);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.resting, 50u);
  m.add(2, kSym, false, 101, 50, false, r);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(m.best_bid(kSym), 100u);
  EXPECT_EQ(m.best_ask(kSym), 101u);
  EXPECT_EQ(m.resting_orders(), 2u);
}

TEST(Matching, TradesAtRestingPrice) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, false, 100, 50, false, r);
  m.add(2, kSym, true, 105, 30, false, r);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].resting_ref, 1u);
  EXPECT_EQ(r.fills[0].shares, 30u);
  EXPECT_EQ(r.fills[0].price, 100u);
  EXPECT_EQ(r.resting, 0u);
  EXPECT_EQ(m.open_shares(1), 20u);
  EXPECT_EQ(m.open_shares(2), 0u);
}

TEST(Matching, PriceThenTimePriority) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, false, 101, 10, false, r);
  m.add(2, kSym, false, 100, 10, false, r);  // better price
  m.add(3, kSym, false, 100, 10, false, r);  // same price, later
  m.add(4, kSym, true, 101, 25, false, r);
  ASSERT_EQ(r.fills.size(), 3u);
  EXPECT_EQ(r.fills[0].resting_ref, 2u);
  EXPECT_EQ(r.fills[1].resting_ref, 3u);
  EXPECT_EQ(r.fills[2].resting_ref, 1u);
  EXPECT_EQ(r.fills[2].shares, 5u);
  EXPECT_EQ(r.fills[2].price, 101u);
  EXPECT_EQ(m.open_shares(1), 5u);
  // Match numbers are unique and increasing.
  EXPECT_LT(r.fills[0].match_number, r.fills[1].match_number);
  EXPECT_LT(r.fills[1].match_number, r.fills[2].match_number);
}

TEST(Matching, RemainderRestsAfterSweep) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, true, 100, 10, false, r);
  m.add(2, kSym, false, 99, 30, false, r);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].price, 100u);
  EXPECT_EQ(r.resting, 20u);
  EXPECT_EQ(m.best_bid(kSym), 0u);
  EXPECT_EQ(m.best_ask(kSym), 99u);
}

TEST(Matching, IocRemainderIsCanceledNotRested) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, false, 100, 10, false, r);
  m.add(2, kSym, true, 100, 25, true, r);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.canceled, 15u);
  EXPECT_EQ(r.resting, 0u);
  EXPECT_EQ(m.best_bid(kSym), 0u);
  EXPECT_EQ(m.resting_orders(), 0u);
}

TEST(Matching, MarketOrderSweepsAndNeverRests) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, true, 100, 10, false, r);
  m.add(2, kSym, true, 90, 10, false, r);
  m.add(3, kSym, false, kMarketPrice, 30, false, r);
  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[0].price, 100u);
  EXPECT_EQ(r.fills[1].price, 90u);
  EXPECT_EQ(r.canceled, 10u);
  EXPECT_EQ(m.resting_orders(), 0u);
}

TEST(Matching, ReduceKeepsPriorityAndRemoveCleansLevel) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, kSym, false, 100, 50, false, r);
  m.add(2, kSym, false, 100, 50, false, r);
  EXPECT_EQ(m.reduce(1, 20), 30u);
  EXPECT_EQ(m.reduce(1, 20), 0u);  // already at or below the target
  EXPECT_EQ(m.reduce(1, 99), 0u);  // cannot increase
  m.add(3, kSym, true, 100, 20, false, r);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].resting_ref, 1u);  // still first in the queue

  EXPECT_EQ(m.reduce(2, 0), 50u);
  EXPECT_EQ(m.best_ask(kSym), 0u);
  EXPECT_EQ(m.reduce(2, 0), 0u);
  EXPECT_EQ(m.reduce(404, 0), 0u);
}

TEST(Matching, SymbolsAreIndependent) {
  SimpleMatchingEngine m;
  AddResult r;
  m.add(1, Symbol("AAPL"), false, 100, 10, false, r);
  m.add(2, Symbol("MSFT"), true, 100, 10, false, r);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(m.resting_orders(), 2u);
}
