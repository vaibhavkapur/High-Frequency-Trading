// Tests for MpscQueue: single-threaded semantics plus a multi-producer stress
// test that checks no loss, no duplication and per-producer FIFO order.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lfm/mpsc_queue.hpp"
#include "test_util.hpp"

using lfm::MpscQueue;
using testutil::Tracked;

namespace {

void rejects_bad_capacity() {
  for (std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{12}}) {
    bool threw = false;
    try {
      MpscQueue<int> q(cap);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
}

void full_and_empty() {
  MpscQueue<int> q(4);
  int v = 7;
  CHECK(!q.try_pop(v));
  CHECK(v == 7);
  for (int i = 0; i < 4; ++i) {
    CHECK(q.try_push(i));
  }
  CHECK(q.size_approx() == 4);
  CHECK(!q.try_push(99));
  for (int i = 0; i < 4; ++i) {
    CHECK(q.try_pop(v));
    CHECK(v == i);
  }
  CHECK(!q.try_pop(v));
}

void wraparound_preserves_order() {
  for (std::size_t cap : {std::size_t{2}, std::size_t{8}}) {
    MpscQueue<std::uint64_t> q(cap);
    std::uint64_t in = 0;
    std::uint64_t out = 0;
    for (int round = 0; round < 5000; ++round) {
      const std::size_t burst = 1 + static_cast<std::size_t>(round) % cap;
      for (std::size_t i = 0; i < burst; ++i) {
        CHECK(q.try_push(in++));
      }
      for (std::size_t i = 0; i < burst; ++i) {
        std::uint64_t v = 0;
        CHECK(q.try_pop(v));
        CHECK(v == out++);
      }
    }
  }
}

void lifetime_is_balanced() {
  CHECK(Tracked::live == 0);
  {
    MpscQueue<Tracked> q(4);
    Tracked out;
    for (std::uint64_t i = 0; i < 10; ++i) {  // wrap with live objects
      CHECK(q.try_emplace(i));
      CHECK(q.try_pop(out));
    }
    CHECK(q.try_emplace(1U));
    CHECK(q.try_emplace(2U));
    CHECK(Tracked::live == 3);
  }
  CHECK(Tracked::live == 0);  // destructor released the two unconsumed values
}

struct Message {
  std::uint32_t producer;
  std::uint32_t check;  // producer ^ low bits of seq
  std::uint64_t seq;
};

void multi_producer_stress(unsigned producers, std::size_t capacity, std::uint64_t per_producer) {
  MpscQueue<Message> q(capacity);
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (unsigned p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::uint64_t i = 0; i < per_producer; ++i) {
        const Message m{p, p ^ static_cast<std::uint32_t>(i), i};
        while (!q.try_push(m)) {
          std::this_thread::yield();
        }
      }
    });
  }
  go.store(true, std::memory_order_release);

  std::vector<std::uint64_t> next(producers, 0);
  std::uint64_t failures = 0;
  const std::uint64_t total = per_producer * producers;
  for (std::uint64_t received = 0; received < total; ++received) {
    Message m{};
    while (!q.try_pop(m)) {
      std::this_thread::yield();
    }
    if (m.producer >= producers || m.check != (m.producer ^ static_cast<std::uint32_t>(m.seq)) ||
        m.seq != next[m.producer]) {
      ++failures;
    } else {
      ++next[m.producer];
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  CHECK(failures == 0);
  for (unsigned p = 0; p < producers; ++p) {
    CHECK(next[p] == per_producer);  // nothing lost, nothing duplicated
  }
  Message m{};
  CHECK(!q.try_pop(m));
}

}  // namespace

int main() {
  std::printf("mpsc_test\n");
  RUN(rejects_bad_capacity);
  RUN(full_and_empty);
  RUN(wraparound_preserves_order);
  RUN(lifetime_is_balanced);

  const std::uint64_t n = testutil::stress_iters(1'000'000);
  struct Case {
    unsigned producers;
    std::size_t capacity;
  };
  for (const Case c : {Case{1, 2}, Case{2, 2}, Case{3, 16}, Case{4, 1024}}) {
    std::printf("  multi_producer_stress producers=%u capacity=%zu\n", c.producers, c.capacity);
    multi_producer_stress(c.producers, c.capacity, n / c.producers);
  }
  std::printf("OK\n");
  return 0;
}
