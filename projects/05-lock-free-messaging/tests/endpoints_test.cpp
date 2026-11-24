// Tests for the Producer/Consumer endpoints (backpressure and drop counters,
// wait policies), the mutex reference queue and the affinity helpers.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "lfm/affinity.hpp"
#include "lfm/endpoints.hpp"
#include "lfm/mpsc_queue.hpp"
#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"
#include "test_util.hpp"

namespace {

void drop_and_full_counters() {
  lfm::SpscQueue<int> q(2);
  lfm::Producer producer(q);
  CHECK(producer.try_push(1));
  CHECK(producer.push_or_drop(2));
  CHECK(!producer.try_push(3));      // full, not counted as a drop
  CHECK(!producer.push_or_drop(4));  // full, dropped
  CHECK(!producer.push_or_drop(5));
  const lfm::ProducerStats s = producer.stats();
  CHECK(s.pushed == 2);
  CHECK(s.full_events == 3);
  CHECK(s.dropped == 2);
  CHECK(s.wait_spins == 0);

  lfm::Consumer consumer(q);
  int v = 0;
  CHECK(consumer.try_pop(v) && v == 1);
  CHECK(consumer.try_pop(v) && v == 2);  // dropped messages never overwrote these
  CHECK(!consumer.try_pop(v));
  const lfm::ConsumerStats c = consumer.stats();
  CHECK(c.popped == 2);
  CHECK(c.empty_events == 1);
}

void stop_predicate_unblocks() {
  lfm::SpscQueue<int> q(1);
  lfm::Producer<lfm::SpscQueue<int>, lfm::BusySpin> producer(q);
  CHECK(producer.try_push(1));
  int budget = 100;
  CHECK(!producer.push(2, [&] { return --budget < 0; }));  // nobody consumes: gives up
  CHECK(producer.stats().wait_spins == 100);
  CHECK(producer.stats().pushed == 1);

  lfm::Consumer<lfm::SpscQueue<int>, lfm::YieldWait> consumer(q);
  int v = 0;
  consumer.pop(v);
  CHECK(v == 1);
  budget = 10;
  CHECK(!consumer.pop(v, [&] { return --budget < 0; }));
  CHECK(consumer.stats().wait_spins == 10);
}

// Blocking push/pop with backpressure across threads, for each wait policy
// and for each queue type the endpoints support.
template <typename Q, typename Wait>
void backpressure_roundtrip(std::uint64_t n) {
  Q q(4);  // small: the producer hits "full" constantly
  lfm::Producer<Q, Wait> producer(q);
  lfm::Consumer<Q, Wait> consumer(q);
  std::thread t([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      producer.push(i);
    }
  });
  std::uint64_t failures = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t v = 0;
    consumer.pop(v);
    failures += v == i ? 0 : 1;
  }
  t.join();
  CHECK(failures == 0);
  CHECK(producer.stats().pushed == n);
  CHECK(producer.stats().dropped == 0);
  CHECK(consumer.stats().popped == n);
  // Every wait episode spins at least once.
  CHECK(producer.stats().wait_spins >= producer.stats().full_events);
}

void mutex_queue_blocking() {
  lfm::MutexQueue<std::uint64_t> q(3);  // non-power-of-two is fine here
  const std::uint64_t n = testutil::stress_iters(200'000);
  std::thread t([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      q.push(i);
    }
  });
  std::uint64_t failures = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t v = 0;
    q.pop(v);
    failures += v == i ? 0 : 1;
  }
  t.join();
  CHECK(failures == 0);
  CHECK(q.size_approx() == 0);
}

void affinity_helpers_do_not_crash() {
  std::printf("    affinity: %s\n", lfm::affinity_support().c_str());
  std::thread t([] {
    // Results are platform-dependent (see affinity.hpp); just exercise them.
    const bool pinned = lfm::pin_current_thread(0);
    const bool hinted = lfm::hint_core_class(lfm::CoreClass::kPerformance);
    std::printf("    pin_current_thread(0)=%d hint_core_class(P)=%d current_cpu=%d\n", pinned,
                hinted, lfm::current_cpu());
#if defined(__linux__)
    CHECK(!pinned || lfm::current_cpu() == 0);
#endif
    CHECK(!lfm::pin_current_thread(-1));
  });
  t.join();
}

}  // namespace

int main() {
  std::printf("endpoints_test\n");
  RUN(drop_and_full_counters);
  RUN(stop_predicate_unblocks);
  const std::uint64_t n = testutil::stress_iters(300'000);
  std::printf("  backpressure_roundtrip (%llu messages each)\n",
              static_cast<unsigned long long>(n));
  backpressure_roundtrip<lfm::SpscQueue<std::uint64_t>, lfm::BusySpin>(n);
  backpressure_roundtrip<lfm::SpscQueue<std::uint64_t>, lfm::YieldWait>(n);
  backpressure_roundtrip<lfm::SpscQueue<std::uint64_t>, lfm::SpinThenYield>(n);
  backpressure_roundtrip<lfm::MpscQueue<std::uint64_t>, lfm::SpinThenYield>(n);
  backpressure_roundtrip<lfm::MutexQueue<std::uint64_t>, lfm::YieldWait>(n);
  RUN(mutex_queue_blocking);
  RUN(affinity_helpers_do_not_crash);
  std::printf("OK\n");
  return 0;
}
