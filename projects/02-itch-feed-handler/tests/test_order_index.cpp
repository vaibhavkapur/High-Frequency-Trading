#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "itch/order_index.hpp"

using namespace itch;

TEMPLATE_TEST_CASE("order index basic operations", "[index]", FlatOrderIndex, StdOrderIndex) {
  TestType idx;
  CHECK(idx.size() == 0);
  CHECK(idx.find(1) == nullptr);
  CHECK_FALSE(idx.erase(1));

  CHECK(idx.insert(1, 10));
  CHECK(idx.insert(0, 20));  // key 0 is an ordinary key
  CHECK_FALSE(idx.insert(1, 99));
  REQUIRE(idx.find(1) != nullptr);
  CHECK(*idx.find(1) == 10);
  REQUIRE(idx.find(0) != nullptr);
  CHECK(*idx.find(0) == 20);
  CHECK(idx.size() == 2);

  CHECK(idx.erase(1));
  CHECK(idx.find(1) == nullptr);
  CHECK_FALSE(idx.erase(1));
  CHECK(idx.size() == 1);
  CHECK(idx.insert(1, 30));
  CHECK(*idx.find(1) == 30);
}

TEMPLATE_TEST_CASE("order index matches a reference map under random churn", "[index][property]",
                   FlatOrderIndex, StdOrderIndex) {
  TestType idx;
  std::unordered_map<OrderRef, std::uint32_t> ref;
  std::mt19937_64 rng(7);
  // A small key space forces frequent re-insertion of erased keys and long
  // probe runs; sequential keys mimic real order reference numbers.
  for (int i = 0; i < 200'000; ++i) {
    const OrderRef key = rng() % 3000;
    const auto value = static_cast<std::uint32_t>(rng() % 1'000'000);
    switch (rng() % 3) {
      case 0:
        REQUIRE(idx.insert(key, value) == ref.emplace(key, value).second);
        break;
      case 1:
        REQUIRE(idx.erase(key) == (ref.erase(key) != 0));
        break;
      default: {
        const std::uint32_t* got = idx.find(key);
        const auto it = ref.find(key);
        REQUIRE((got != nullptr) == (it != ref.end()));
        if (got != nullptr) REQUIRE(*got == it->second);
      }
    }
    REQUIRE(idx.size() == ref.size());
  }
  for (const auto& [key, value] : ref) {
    REQUIRE(idx.find(key) != nullptr);
    REQUIRE(*idx.find(key) == value);
  }
}

TEST_CASE("flat index grows and keeps every entry", "[index]") {
  FlatOrderIndex idx(16);
  const std::size_t initial = idx.capacity();
  for (std::uint32_t i = 0; i < 50'000; ++i) REQUIRE(idx.insert(OrderRef{i} * 7919, i));
  CHECK(idx.capacity() > initial);
  CHECK(idx.size() == 50'000);
  for (std::uint32_t i = 0; i < 50'000; ++i) {
    const std::uint32_t* v = idx.find(OrderRef{i} * 7919);
    REQUIRE(v != nullptr);
    REQUIRE(*v == i);
  }
  // Erase everything in a different order than insertion.
  for (std::uint32_t i = 50'000; i-- > 0;) REQUIRE(idx.erase(OrderRef{i} * 7919));
  CHECK(idx.size() == 0);
}

TEST_CASE("flat index erase keeps colliding keys reachable", "[index]") {
  // With 16 slots, many of these keys share home slots and wrap around the
  // end of the table, exercising backward-shift deletion across the wrap.
  FlatOrderIndex idx(16);
  std::vector<OrderRef> keys;
  for (OrderRef k = 1; k <= 11; ++k) keys.push_back(k * 0x1000000000000ull);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    REQUIRE(idx.insert(keys[i], static_cast<std::uint32_t>(i)));
  }
  for (std::size_t gone = 0; gone < keys.size(); ++gone) {
    REQUIRE(idx.erase(keys[gone]));
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const std::uint32_t* v = idx.find(keys[i]);
      if (i <= gone) {
        REQUIRE(v == nullptr);
      } else {
        REQUIRE(v != nullptr);
        REQUIRE(*v == i);
      }
    }
  }
}
