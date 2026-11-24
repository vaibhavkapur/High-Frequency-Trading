#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "lpl/branch_and_sharing.hpp"
#include "lpl/spsc_handoff.hpp"

namespace {

#define REQUIRE(cond)                                                                 \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << "FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #cond << "\n"; \
      std::exit(1);                                                                   \
    }                                                                                 \
  } while (0)

void test_spsc_edges() {
  // Invalid capacities must throw.
  for (size_t bad : {0UL, 1UL, 3UL, 100UL}) {
    bool threw = false;
    try {
      lpl::SpscRingBuffer<uint64_t> q(bad);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    REQUIRE(threw);
  }

  lpl::SpscRingBuffer<uint64_t> q(4);  // holds 3 items (1 slot distinguishes full vs empty)
  uint64_t v = 0;
  REQUIRE(q.empty());
  REQUIRE(!q.try_pop(v));
  REQUIRE(q.try_push(10));
  REQUIRE(q.try_push(20));
  REQUIRE(q.try_push(30));
  REQUIRE(!q.try_push(40));  // full
  REQUIRE(q.try_pop(v) && v == 10);
  REQUIRE(q.try_push(40));   // wraparound
  REQUIRE(q.try_pop(v) && v == 20);
  REQUIRE(q.try_pop(v) && v == 30);
  REQUIRE(q.try_pop(v) && v == 40);
  REQUIRE(q.empty());
}

void test_spsc_multithreaded_fifo() {
  constexpr size_t kCount = 200'000;
  lpl::SpscRingBuffer<uint64_t> q(1024);
  bool order_ok = true;

  std::thread consumer([&]() {
    uint64_t expected = 1;
    uint64_t val = 0;
    while (expected <= kCount) {
      if (q.try_pop(val)) {
        if (val != expected) {
          order_ok = false;
          return;
        }
        ++expected;
      } else {
        lpl::cpu_relax();
      }
    }
  });

  for (uint64_t i = 1; i <= kCount; ++i) {
    while (!q.try_push(i)) {
      lpl::cpu_relax();
    }
  }
  consumer.join();
  REQUIRE(order_ok);
  REQUIRE(q.empty());

  const uint64_t expected_sum = (kCount * (kCount + 1)) / 2;
  REQUIRE(lpl::run_spsc_burst_handoff(kCount, 2048, false) == expected_sum);
}

void test_handoff_strategies_equivalence() {
  const auto m_spin =
      lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kBusySpin, 300, 50, 0, false);
  const auto m_hybrid =
      lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kHybridSpinYield, 300, 50, 0, false);
  const auto m_block =
      lpl::measure_handoff_ping_pong(lpl::WaitStrategy::kBlockingCondVar, 300, 50, 0, false);

  REQUIRE(m_spin.checksum == m_hybrid.checksum);
  REQUIRE(m_spin.checksum == m_block.checksum);
  REQUIRE(m_spin.rtt_half_ns.count == 300);
  REQUIRE(m_block.rtt_half_ns.count == 300);
}

void test_false_sharing_correctness() {
  const size_t threads = 4;
  const size_t iters = 50'000;
  const uint64_t expected = threads * iters;
  REQUIRE(lpl::run_false_sharing_counters<8>(threads, iters, false) == expected);
  REQUIRE(lpl::run_false_sharing_counters<64>(threads, iters, false) == expected);
  REQUIRE(lpl::run_false_sharing_counters<128>(threads, iters, false) == expected);
}

}  // namespace

int main() {
  test_spsc_edges();
  test_spsc_multithreaded_fifo();
  test_handoff_strategies_equivalence();
  test_false_sharing_correctness();
  std::cout << "test_concurrency: ALL PASSED\n";
  return 0;
}
