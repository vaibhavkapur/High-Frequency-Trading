#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "itch/replay.hpp"

using namespace itch;

TEST_CASE("unlimited pacing never delays", "[replay]") {
  Pacer p = Pacer::unlimited();
  CHECK(p.due_ns(0, 0) == 0);
  CHECK(p.due_ns(1'000'000, 999) == 0);
}

TEST_CASE("fixed-rate pacing spaces messages evenly", "[replay]") {
  Pacer p = Pacer::fixed_rate(1000.0);  // 1 ms apart
  CHECK(p.due_ns(0, 0) == 0);
  CHECK(p.due_ns(1, 0) == 1'000'000);
  CHECK(p.due_ns(2500, 0) == 2'500'000'000ull);
}

TEST_CASE("time-scaled pacing follows ITCH timestamps", "[replay]") {
  Pacer p = Pacer::time_scaled(10.0);  // 10x faster than real time
  const Timestamp open = 34'200'000'000'000ull;
  CHECK(p.due_ns(0, open) == 0);  // the first message anchors the timeline
  CHECK(p.due_ns(1, open + 1'000'000'000ull) == 100'000'000ull);
  // A timestamp that steps backwards is released immediately.
  CHECK(p.due_ns(2, open - 5) == 0);
  CHECK(p.due_ns(3, open + 20'000'000'000ull) == 2'000'000'000ull);
}

TEST_CASE("wait() actually throttles", "[replay]") {
  Pacer p = Pacer::fixed_rate(200.0);  // 5 ms apart
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i <= 4; ++i) p.wait(i, 0);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  // Message 4 is due 20 ms after the first; sleeping can only overshoot.
  CHECK(elapsed >= std::chrono::milliseconds(20));
}
