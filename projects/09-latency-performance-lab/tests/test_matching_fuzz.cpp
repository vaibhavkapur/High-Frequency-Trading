#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <vector>

#include "lpl/matching_engine.hpp"

namespace {

#define REQUIRE(cond)                                                                 \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #cond << "\n"; \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (0)

void verify_engines_identical(const std::vector<lpl::OrderCommand>& cmds) {
  lpl::BaselineMatchingEngine eng_a;
  lpl::PooledFlatMatchingEngine eng_b(std::max<size_t>(1024, cmds.size() + 64));

  std::vector<lpl::Trade> trades_a;
  std::vector<lpl::Trade> trades_b;
  for (const auto& cmd : cmds) {
    eng_a.process(cmd, &trades_a);
    eng_b.process(cmd, &trades_b);
  }
  REQUIRE(trades_a == trades_b);
  REQUIRE(eng_a.summary() == eng_b.summary());
}

#ifndef LPL_USE_LIBFUZZER

void test_matching_rules_unit() {
  // Price-time FIFO priority, partial fills, cancel, modify down, rejects.
  const std::vector<lpl::OrderCommand> cmds = {
      // Two resting asks at 100 (FIFO order: id 1 then id 2) and one at 101 (id 3)
      {lpl::OrderAction::kAddLimit, lpl::Side::kSell, 1, 100, 50},
      {lpl::OrderAction::kAddLimit, lpl::Side::kSell, 2, 100, 75},
      {lpl::OrderAction::kAddLimit, lpl::Side::kSell, 3, 101, 40},
      // Duplicate ID must be rejected
      {lpl::OrderAction::kAddLimit, lpl::Side::kBuy, 1, 99, 10},
      // Zero qty & non-positive price must be rejected
      {lpl::OrderAction::kAddLimit, lpl::Side::kBuy, 4, 99, 0},
      {lpl::OrderAction::kAddLimit, lpl::Side::kBuy, 5, 0, 10},
      // Modify order 2 down from 75 -> 60
      {lpl::OrderAction::kModifyQty, lpl::Side::kSell, 2, 0, 60},
      // Modify up (60 -> 80) must be rejected
      {lpl::OrderAction::kModifyQty, lpl::Side::kSell, 2, 0, 80},
      // Aggressive buy sweeps order 1 (50) and partial order 2 (30 of 60)
      {lpl::OrderAction::kAddLimit, lpl::Side::kBuy, 10, 100, 80},
      // Cancel remaining qty on order 2
      {lpl::OrderAction::kCancel, lpl::Side::kSell, 2, 0, 0},
      // Cancel already cancelled order 2 -> miss
      {lpl::OrderAction::kCancel, lpl::Side::kSell, 2, 0, 0},
      // Market buy sweeps remaining ask at 101 (40)
      {lpl::OrderAction::kAddMarket, lpl::Side::kBuy, 11, 0, 100},
      // Resting bid + modify to 0 acts as cancel
      {lpl::OrderAction::kAddLimit, lpl::Side::kBuy, 12, 98, 25},
      {lpl::OrderAction::kModifyQty, lpl::Side::kBuy, 12, 0, 0},
  };

  lpl::BaselineMatchingEngine a;
  lpl::PooledFlatMatchingEngine b(1024);
  std::vector<lpl::Trade> ta, tb;
  for (const auto& c : cmds) {
    a.process(c, &ta);
    b.process(c, &tb);
  }

  REQUIRE(ta.size() == 3);
  REQUIRE((ta[0] == lpl::Trade{10, 1, 100, 50}));
  REQUIRE((ta[1] == lpl::Trade{10, 2, 100, 30}));
  REQUIRE((ta[2] == lpl::Trade{11, 3, 101, 40}));
  REQUIRE(ta == tb);
  REQUIRE(a.summary() == b.summary());
  REQUIRE(a.summary().resting_orders == 0);
  REQUIRE(a.summary().orders_rejected == 4);
  REQUIRE(a.summary().cancels_succeeded == 2);
  REQUIRE(a.summary().cancels_missed == 1);
}

void test_binary_codec_and_negative_cases() {
  const auto cmds = lpl::generate_matching_workload(500, 101);
  const auto wire = lpl::encode_order_stream(cmds);
  const auto dec = lpl::decode_order_stream(wire);
  REQUIRE(dec.ok);
  REQUIRE(dec.commands.size() == cmds.size());
  for (size_t i = 0; i < cmds.size(); ++i) {
    REQUIRE(dec.commands[i].action == cmds[i].action);
    REQUIRE(dec.commands[i].side == cmds[i].side);
    REQUIRE(dec.commands[i].order_id == cmds[i].order_id);
    REQUIRE(dec.commands[i].price == cmds[i].price);
    REQUIRE(dec.commands[i].qty == cmds[i].qty);
  }

  // Negative cases: empty, truncated, bad magic, bad version, trailing byte, bad action/side.
  REQUIRE(!lpl::decode_order_stream({}).ok);
  REQUIRE(!lpl::decode_order_stream(std::span<const uint8_t>(wire.data(), 10)).ok);

  {
    auto bad_magic = wire;
    bad_magic[0] = 'X';
    REQUIRE(!lpl::decode_order_stream(bad_magic).ok);
  }
  {
    auto bad_ver = wire;
    bad_ver[4] = 99;
    REQUIRE(!lpl::decode_order_stream(bad_ver).ok);
  }
  {
    auto trailing = wire;
    trailing.push_back(0);
    REQUIRE(!lpl::decode_order_stream(trailing).ok);
  }
  {
    auto bad_action = wire;
    bad_action[16] = 9;
    REQUIRE(!lpl::decode_order_stream(bad_action).ok);
  }
  {
    auto bad_side = wire;
    bad_side[17] = 2;
    REQUIRE(!lpl::decode_order_stream(bad_side).ok);
  }

  // Fuzz 2,000 random/mutated binary blobs: decoder must never crash or invoke UB,
  // and any valid decoded stream must execute identically on both engines.
  std::mt19937_64 rng(314159);
  for (size_t iter = 0; iter < 2000; ++iter) {
    const size_t len = rng() % 512;
    std::vector<uint8_t> blob(len);
    for (size_t i = 0; i < len; ++i) {
      blob[i] = static_cast<uint8_t>(rng() & 0xFFU);
    }
    // Occasionally stamp valid magic/version to penetrate deeper into record parsing.
    if (len >= 16 && (iter % 3 == 0)) {
      blob[0] = 'L';
      blob[1] = 'P';
      blob[2] = 'L';
      blob[3] = 'O';
      blob[4] = 1;
      blob[5] = 0;
    }
    const auto res = lpl::decode_order_stream(blob);
    if (res.ok) {
      verify_engines_identical(res.commands);
    }
  }
}

void test_differential_property_fuzz() {
  for (uint64_t seed = 1; seed <= 50; ++seed) {
    const auto cmds = lpl::generate_matching_workload(1500, seed * 7919ULL);
    verify_engines_identical(cmds);
  }
}

#endif  // LPL_USE_LIBFUZZER

}  // namespace

#ifdef LPL_USE_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const auto res = lpl::decode_order_stream(std::span<const uint8_t>(data, size));
  if (res.ok) {
    verify_engines_identical(res.commands);
  }
  return 0;
}
#else
int main() {
  test_matching_rules_unit();
  test_binary_codec_and_negative_cases();
  test_differential_property_fuzz();
  std::cout << "test_matching_fuzz: ALL PASSED\n";
  return 0;
}
#endif
