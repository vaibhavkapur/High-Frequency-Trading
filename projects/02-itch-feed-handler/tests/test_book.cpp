#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "itch/book.hpp"

using namespace itch;

namespace {

constexpr std::uint16_t kLoc = 7;

template <class Index>
std::vector<LevelView> top(const BookBuilder<Index>& b, std::uint16_t locate, Side side,
                           std::size_t n = 10) {
  std::vector<LevelView> out;
  b.top(locate, side, n, out);
  return out;
}

}  // namespace

TEMPLATE_TEST_CASE("add -> partial execution -> cancel -> delete leaves the expected residual",
                   "[book]", FlatOrderIndex, StdOrderIndex) {
  BookBuilder<TestType> b;
  REQUIRE(b.add(1, kLoc, Side::Buy, 1000, 100'0000).status == BookStatus::Ok);
  CHECK(b.bbo(kLoc).bid_qty == 1000);

  // Partial execution of 300.
  const BookResult exec = b.reduce(1, 300);
  CHECK(exec.status == BookStatus::Ok);
  CHECK(exec.qty == 300);
  CHECK(exec.price == 100'0000);
  CHECK(exec.side == Side::Buy);
  CHECK(b.find(1)->qty == 700);
  CHECK(b.bbo(kLoc).bid_qty == 700);

  // Partial cancel of 200.
  CHECK(b.reduce(1, 200).status == BookStatus::Ok);
  CHECK(b.find(1)->qty == 500);
  CHECK(top(b, kLoc, Side::Buy) == std::vector<LevelView>{{100'0000, 500, 1}});

  // Delete removes the residual 500 and the now-empty level.
  const BookResult del = b.remove(1);
  CHECK(del.status == BookStatus::Ok);
  CHECK(del.qty == 500);
  CHECK_FALSE(b.find(1).has_value());
  CHECK_FALSE(b.bbo(kLoc).has_bid);
  CHECK(top(b, kLoc, Side::Buy).empty());
  CHECK(b.live_orders() == 0);
}

TEMPLATE_TEST_CASE("full execution removes the order and its level", "[book]", FlatOrderIndex,
                   StdOrderIndex) {
  BookBuilder<TestType> b;
  b.add(1, kLoc, Side::Sell, 100, 50'0000);
  b.add(2, kLoc, Side::Sell, 200, 51'0000);
  CHECK(b.reduce(1, 100).status == BookStatus::Ok);
  CHECK_FALSE(b.find(1).has_value());
  const Bbo bbo = b.bbo(kLoc);
  CHECK(bbo.has_ask);
  CHECK(bbo.ask_price == 51'0000);
  CHECK(bbo.ask_qty == 200);
}

TEMPLATE_TEST_CASE("best bid/offer, spread and depth ordering", "[book]", FlatOrderIndex,
                   StdOrderIndex) {
  BookBuilder<TestType> b;
  b.add(1, kLoc, Side::Buy, 100, 99'0000);
  b.add(2, kLoc, Side::Buy, 200, 99'5000);
  b.add(3, kLoc, Side::Buy, 300, 98'0000);
  b.add(4, kLoc, Side::Buy, 50, 99'5000);
  b.add(5, kLoc, Side::Sell, 400, 100'5000);
  b.add(6, kLoc, Side::Sell, 500, 100'0000);
  b.add(7, kLoc, Side::Sell, 600, 101'0000);

  const Bbo bbo = b.bbo(kLoc);
  CHECK(bbo.bid_price == 99'5000);
  CHECK(bbo.bid_qty == 250);
  CHECK(bbo.ask_price == 100'0000);
  CHECK(bbo.ask_qty == 500);
  CHECK(spread(bbo) == 5000);

  // Bids best-first are descending, asks best-first are ascending.
  CHECK(top(b, kLoc, Side::Buy) ==
        std::vector<LevelView>{{99'5000, 250, 2}, {99'0000, 100, 1}, {98'0000, 300, 1}});
  CHECK(top(b, kLoc, Side::Sell) ==
        std::vector<LevelView>{{100'0000, 500, 1}, {100'5000, 400, 1}, {101'0000, 600, 1}});
  // Top-N truncates.
  CHECK(top(b, kLoc, Side::Buy, 2) == std::vector<LevelView>{{99'5000, 250, 2}, {99'0000, 100, 1}});
}

TEST_CASE("derived top-of-book metrics", "[book]") {
  Bbo bbo;
  CHECK_FALSE(spread(bbo).has_value());
  CHECK_FALSE(imbalance(bbo).has_value());
  CHECK_FALSE(microprice(bbo).has_value());

  bbo.has_bid = bbo.has_ask = true;
  bbo.bid_price = 100'0000;
  bbo.ask_price = 100'0200;
  bbo.bid_qty = 300;
  bbo.ask_qty = 100;
  CHECK(*imbalance(bbo) == 0.5);
  // (bid_px * ask_qty + ask_px * bid_qty) / (bid_qty + ask_qty): more bid
  // size pushes the estimate toward the ask.
  CHECK(*microprice(bbo) == 100'0150.0);
}

TEMPLATE_TEST_CASE("replace assigns a new id and loses time priority", "[book]", FlatOrderIndex,
                   StdOrderIndex) {
  BookBuilder<TestType> b;
  b.add(1, kLoc, Side::Buy, 100, 10'0000);
  b.add(2, kLoc, Side::Buy, 100, 10'0000);
  b.add(3, kLoc, Side::Buy, 100, 10'0000);
  CHECK(b.queue_position(1) == 0);
  CHECK(b.queue_position(2) == 1);
  CHECK(b.queue_position(3) == 2);

  SECTION("same price: goes to the back of the same level") {
    const BookResult r = b.replace(1, 4, 150, 10'0000);
    CHECK(r.status == BookStatus::Ok);
    CHECK(r.locate == kLoc);
    CHECK(r.side == Side::Buy);
    CHECK_FALSE(b.find(1).has_value());  // the old id is gone
    REQUIRE(b.find(4).has_value());
    CHECK(b.find(4)->qty == 150);
    CHECK(b.find(4)->side == Side::Buy);  // side and symbol carry over
    CHECK(b.find(4)->locate == kLoc);
    CHECK(b.queue_position(2) == 0);
    CHECK(b.queue_position(3) == 1);
    CHECK(b.queue_position(4) == 2);
    CHECK(top(b, kLoc, Side::Buy) == std::vector<LevelView>{{10'0000, 350, 3}});
  }

  SECTION("new price: leaves the old level, joins the back of the new one") {
    b.add(5, kLoc, Side::Buy, 100, 11'0000);
    CHECK(b.replace(2, 6, 70, 11'0000).status == BookStatus::Ok);
    CHECK(b.queue_position(6) == 1);  // behind order 5
    CHECK(b.queue_position(1) == 0);
    CHECK(b.queue_position(3) == 1);
    CHECK(top(b, kLoc, Side::Buy) == std::vector<LevelView>{{11'0000, 170, 2}, {10'0000, 200, 2}});
  }

  SECTION("the old id cannot be used again; a chain of replaces keeps one order") {
    REQUIRE(b.replace(1, 4, 100, 10'0000).status == BookStatus::Ok);
    CHECK(b.reduce(1, 10).status == BookStatus::UnknownOrder);
    CHECK(b.remove(1).status == BookStatus::UnknownOrder);
    REQUIRE(b.replace(4, 5, 100, 12'0000).status == BookStatus::Ok);
    REQUIRE(b.replace(5, 6, 40, 9'0000).status == BookStatus::Ok);
    CHECK(b.live_orders() == 3);
    CHECK(b.find(6)->price == 9'0000);
    CHECK(b.counters().replaces == 3);
    CHECK(b.counters().adds == 3);  // replaces are not counted as adds/removes
    CHECK(b.counters().removes == 0);
  }

  SECTION("replace of an unknown id is ignored") {
    CHECK(b.replace(99, 100, 10, 10'0000).status == BookStatus::UnknownOrder);
    CHECK_FALSE(b.find(100).has_value());
    CHECK(b.live_orders() == 3);
  }

  SECTION("replace onto a live id removes the original and rejects the new order") {
    CHECK(b.replace(1, 2, 10, 10'0000).status == BookStatus::DuplicateOrder);
    CHECK_FALSE(b.find(1).has_value());
    CHECK(b.find(2)->qty == 100);
    CHECK(b.live_orders() == 2);
  }
}

TEMPLATE_TEST_CASE("inconsistent input is counted, not fatal", "[book][negative]", FlatOrderIndex,
                   StdOrderIndex) {
  BookBuilder<TestType> b;
  REQUIRE(b.add(1, kLoc, Side::Buy, 100, 10'0000).status == BookStatus::Ok);

  // Duplicate order reference: rejected, original untouched.
  CHECK(b.add(1, kLoc, Side::Sell, 999, 20'0000).status == BookStatus::DuplicateOrder);
  CHECK(b.find(1)->qty == 100);
  CHECK_FALSE(b.bbo(kLoc).has_ask);

  // Unknown references.
  CHECK(b.reduce(42, 10).status == BookStatus::UnknownOrder);
  CHECK(b.remove(42).status == BookStatus::UnknownOrder);

  // Executing more than remains is clamped and removes the order.
  const BookResult over = b.reduce(1, 150);
  CHECK(over.status == BookStatus::OverReduce);
  CHECK(over.qty == 100);
  CHECK_FALSE(b.find(1).has_value());
  CHECK_FALSE(b.bbo(kLoc).has_bid);

  CHECK(b.counters().duplicate_order == 1);
  CHECK(b.counters().unknown_order == 2);
  CHECK(b.counters().over_reduce == 1);

  // Queries on a locate that never appeared are safe.
  CHECK(b.book(60000) == nullptr);
  CHECK_FALSE(b.bbo(60000).has_bid);
  CHECK(top(b, 60000, Side::Buy).empty());
}

TEMPLATE_TEST_CASE("order ids are feed-wide; symbols are isolated by locate", "[book]",
                   FlatOrderIndex, StdOrderIndex) {
  BookBuilder<TestType> b;
  b.add(1, 10, Side::Buy, 100, 5'0000);
  b.add(2, 20, Side::Buy, 200, 6'0000);
  // The same reference number on a different symbol is still a duplicate.
  CHECK(b.add(1, 20, Side::Buy, 1, 1'0000).status == BookStatus::DuplicateOrder);
  CHECK(b.bbo(10).bid_qty == 100);
  CHECK(b.bbo(20).bid_qty == 200);
  // An operation needs only the order id; it lands on the right symbol.
  CHECK(b.remove(2).locate == 20);
  CHECK(b.bbo(10).has_bid);
  CHECK_FALSE(b.bbo(20).has_bid);
}

TEMPLATE_TEST_CASE("freed order slots are reused", "[book]", FlatOrderIndex, StdOrderIndex) {
  BookBuilder<TestType> b;
  for (OrderRef round = 0; round < 1000; ++round) {
    b.add(round * 2, kLoc, Side::Buy, 10, 10'0000);
    b.add(round * 2 + 1, kLoc, Side::Sell, 10, 11'0000);
    b.remove(round * 2);
    b.reduce(round * 2 + 1, 10);
  }
  CHECK(b.live_orders() == 0);
  CHECK_FALSE(b.bbo(kLoc).has_bid);
  CHECK_FALSE(b.bbo(kLoc).has_ask);
}

// ---- property test against a deliberately naive reference model -------------

namespace {

struct RefOrder {
  std::uint16_t locate;
  Side side;
  Price4 price;
  std::uint32_t qty;
};

struct RefModel {
  std::unordered_map<OrderRef, RefOrder> orders;
  // (locate, side, price) -> order ids in arrival order
  std::map<std::tuple<std::uint16_t, char, Price4>, std::vector<OrderRef>> queues;

  static std::tuple<std::uint16_t, char, Price4> key(const RefOrder& o) {
    return {o.locate, static_cast<char>(o.side), o.price};
  }

  bool add(OrderRef ref, const RefOrder& o) {
    if (!orders.emplace(ref, o).second) return false;
    queues[key(o)].push_back(ref);
    return true;
  }
  bool remove(OrderRef ref) {
    const auto it = orders.find(ref);
    if (it == orders.end()) return false;
    auto& q = queues[key(it->second)];
    q.erase(std::find(q.begin(), q.end(), ref));
    if (q.empty()) queues.erase(key(it->second));
    orders.erase(it);
    return true;
  }
  bool reduce(OrderRef ref, std::uint32_t qty) {
    const auto it = orders.find(ref);
    if (it == orders.end()) return false;
    if (qty >= it->second.qty) return remove(ref);
    it->second.qty -= qty;
    return true;
  }

  std::vector<LevelView> levels(std::uint16_t locate, Side side) const {
    std::vector<LevelView> out;
    for (const auto& [k, q] : queues) {
      if (std::get<0>(k) != locate || std::get<1>(k) != static_cast<char>(side)) continue;
      LevelView lv{std::get<2>(k), 0, static_cast<std::uint32_t>(q.size())};
      for (const OrderRef r : q) lv.qty += orders.at(r).qty;
      out.push_back(lv);
    }
    // std::map iterates prices ascending; bids are wanted best (highest) first.
    if (side == Side::Buy) std::reverse(out.begin(), out.end());
    return out;
  }
};

}  // namespace

TEMPLATE_TEST_CASE("random operation sequences match the reference model", "[book][property]",
                   FlatOrderIndex, StdOrderIndex) {
  for (const std::uint64_t seed : {1ull, 2ull, 3ull}) {
    BookBuilder<TestType> b;
    RefModel ref;
    std::mt19937_64 rng(seed);
    std::vector<OrderRef> known;  // ids ever issued, live or not
    OrderRef next_ref = 1;

    const auto random_known = [&]() -> OrderRef {
      // Mostly real ids, sometimes one that never existed.
      if (known.empty() || rng() % 20 == 0) return 1'000'000 + rng() % 100;
      return known[rng() % known.size()];
    };

    for (int step = 0; step < 30'000; ++step) {
      const auto locate = static_cast<std::uint16_t>(1 + rng() % 4);
      const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
      const auto price = static_cast<Price4>(100'0000 + (rng() % 12) * 100);
      const auto qty = static_cast<std::uint32_t>(1 + rng() % 500);
      switch (rng() % 10) {
        case 0:
        case 1:
        case 2:
        case 3: {
          // Occasionally reuse an id to exercise duplicate handling.
          const OrderRef r = !known.empty() && rng() % 25 == 0 ? random_known() : next_ref++;
          const bool ok = ref.add(r, {locate, side, price, qty});
          REQUIRE((b.add(r, locate, side, qty, price).status == BookStatus::Ok) == ok);
          known.push_back(r);
          break;
        }
        case 4:
        case 5: {
          const OrderRef r = random_known();
          const bool ok = ref.reduce(r, qty);
          REQUIRE((b.reduce(r, qty).status != BookStatus::UnknownOrder) == ok);
          break;
        }
        case 6:
        case 7: {
          const OrderRef r = random_known();
          REQUIRE((b.remove(r).status == BookStatus::Ok) == ref.remove(r));
          break;
        }
        default: {
          const OrderRef old_ref = random_known();
          const OrderRef new_ref = next_ref++;
          const auto it = ref.orders.find(old_ref);
          if (it == ref.orders.end()) {
            REQUIRE(b.replace(old_ref, new_ref, qty, price).status == BookStatus::UnknownOrder);
          } else {
            const RefOrder old = it->second;
            ref.remove(old_ref);
            ref.add(new_ref, {old.locate, old.side, price, qty});
            REQUIRE(b.replace(old_ref, new_ref, qty, price).status == BookStatus::Ok);
            known.push_back(new_ref);
          }
        }
      }

      if (step % 500 == 0 || step == 29'999) {
        REQUIRE(b.live_orders() == ref.orders.size());
        for (std::uint16_t loc = 1; loc <= 4; ++loc) {
          for (const Side s : {Side::Buy, Side::Sell}) {
            REQUIRE(top(b, loc, s, 1000) == ref.levels(loc, s));
          }
        }
        // Time priority: every order's queue position agrees.
        for (const auto& [k, q] : ref.queues) {
          for (std::size_t i = 0; i < q.size(); ++i) {
            REQUIRE(b.queue_position(q[i]) == i);
            REQUIRE(b.find(q[i])->qty == ref.orders.at(q[i]).qty);
          }
        }
      }
    }
  }
}

TEMPLATE_TEST_CASE("digest reflects size, price and priority", "[book]", FlatOrderIndex,
                   StdOrderIndex) {
  const auto build = [](const std::function<void(BookBuilder<TestType>&)>& f) {
    BookBuilder<TestType> b;
    f(b);
    return b.digest();
  };
  const auto base = build([](auto& b) {
    b.add(1, kLoc, Side::Buy, 100, 10'0000);
    b.add(2, kLoc, Side::Buy, 100, 10'0000);
  });
  CHECK(base == build([](auto& b) {
          b.add(1, kLoc, Side::Buy, 100, 10'0000);
          b.add(2, kLoc, Side::Buy, 100, 10'0000);
        }));
  // Same aggregate depth, different queue order.
  CHECK(base != build([](auto& b) {
          b.add(2, kLoc, Side::Buy, 100, 10'0000);
          b.add(1, kLoc, Side::Buy, 100, 10'0000);
        }));
  CHECK(base != build([](auto& b) {
          b.add(1, kLoc, Side::Buy, 100, 10'0000);
          b.add(2, kLoc, Side::Buy, 101, 10'0000);
        }));
  CHECK(base != build([](auto& b) {
          b.add(1, kLoc, Side::Sell, 100, 10'0000);
          b.add(2, kLoc, Side::Sell, 100, 10'0000);
        }));
}
