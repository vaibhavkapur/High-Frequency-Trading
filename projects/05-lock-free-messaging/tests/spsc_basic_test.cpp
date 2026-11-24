// Single-threaded behavioural tests for SpscQueue: full/empty outcomes,
// ordering, wraparound, batch APIs, object lifetime and layout.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lfm/spsc_queue.hpp"
#include "test_util.hpp"

using lfm::SpscQueue;
using testutil::Tracked;

namespace {

void rejects_bad_capacity() {
  for (std::size_t cap : {std::size_t{0}, std::size_t{3}, std::size_t{6}, std::size_t{1000}}) {
    bool threw = false;
    try {
      SpscQueue<int> q(cap);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  SpscQueue<int> ok(1);
  CHECK(ok.capacity() == 1);
}

void empty_pop_fails() {
  SpscQueue<int> q(8);
  int v = 42;
  CHECK(!q.try_pop(v));
  CHECK(v == 42);  // untouched on failure
  CHECK(!q.try_pop().has_value());
  CHECK(q.empty_approx());
}

void full_push_fails_without_overwrite() {
  SpscQueue<int> q(4);
  for (int i = 0; i < 4; ++i) {
    CHECK(q.try_push(i));
  }
  CHECK(q.size_approx() == 4);  // every slot is usable
  CHECK(!q.try_push(99));
  CHECK(!q.try_push(100));
  for (int i = 0; i < 4; ++i) {
    int v = -1;
    CHECK(q.try_pop(v));
    CHECK(v == i);  // unread data was not overwritten
  }
  int v = -1;
  CHECK(!q.try_pop(v));
}

void capacity_one() {
  SpscQueue<int> q(1);
  for (int i = 0; i < 100; ++i) {
    CHECK(q.try_push(i));
    CHECK(!q.try_push(i));
    int v = -1;
    CHECK(q.try_pop(v));
    CHECK(v == i);
    CHECK(!q.try_pop(v));
  }
}

void wraparound_preserves_order() {
  // Varying fill levels so the ring index crosses the wrap point at every
  // possible offset, over many multiples of the capacity.
  SpscQueue<std::uint64_t> q(8);
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 10'000; ++round) {
    const int burst = 1 + round % 8;
    for (int i = 0; i < burst; ++i) {
      CHECK(q.try_push(next_in++));
    }
    for (int i = 0; i < burst; ++i) {
      std::uint64_t v = 0;
      CHECK(q.try_pop(v));
      CHECK(v == next_out++);
    }
  }
  CHECK(next_out > 8 * 1000);
  CHECK(q.empty_approx());
}

void batch_push_pop() {
  SpscQueue<int> q(8);
  std::array<int, 12> in{};
  for (int i = 0; i < 12; ++i) {
    in[static_cast<std::size_t>(i)] = i;
  }
  CHECK(q.try_push_batch(in) == 8);  // partial: only 8 fit
  CHECK(q.try_push_batch(in) == 0);

  std::array<int, 5> out{};
  CHECK(q.try_pop_batch(out) == 5);
  for (int i = 0; i < 5; ++i) {
    CHECK(out[static_cast<std::size_t>(i)] == i);
  }

  // Now head=5, tail=8: a 5-item push must split across the wrap point.
  CHECK(q.try_push_batch(std::span<const int>(in).subspan(0, 5)) == 5);
  std::array<int, 16> big{};
  CHECK(q.try_pop_batch(big) == 8);
  const std::array<int, 8> want{5, 6, 7, 0, 1, 2, 3, 4};
  for (std::size_t i = 0; i < 8; ++i) {
    CHECK(big[i] == want[i]);
  }
  CHECK(q.try_pop_batch(big) == 0);
  CHECK(q.try_push_batch(std::span<const int>{}) == 0);
}

void consume_in_place() {
  SpscQueue<int> q(8);
  for (int i = 0; i < 6; ++i) {
    CHECK(q.try_push(i));
  }
  std::vector<int> seen;
  CHECK(q.consume([&](int&& v) { seen.push_back(v); }, 4) == 4);
  CHECK(q.size_approx() == 2);
  CHECK(q.consume([&](int&& v) { seen.push_back(v); }) == 2);
  CHECK(q.consume([&](int&&) { CHECK(false); }) == 0);
  CHECK(seen == (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

void consume_handles_throwing_callback() {
  SpscQueue<Tracked> q(8);
  for (std::uint64_t i = 0; i < 5; ++i) {
    CHECK(q.try_emplace(i));
  }
  bool threw = false;
  try {
    q.consume([](Tracked&& t) {
      if (t.value == 2) {
        throw std::runtime_error("boom");
      }
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  // Items 0,1 and the one that threw (2) are gone; 3 and 4 remain, intact.
  CHECK(q.size_approx() == 2);
  CHECK(Tracked::live == 2);
  Tracked t;
  CHECK(q.try_pop(t) && t.value == 3);
  CHECK(q.try_pop(t) && t.value == 4);
}

void lifetime_is_balanced() {
  CHECK(Tracked::live == 0);
  {
    SpscQueue<Tracked> q(4);
    CHECK(q.try_emplace(1U));
    CHECK(q.try_emplace(2U));
    CHECK(q.try_push(Tracked{3}));
    CHECK(Tracked::live == 3);
    Tracked out;
    CHECK(q.try_pop(out));
    CHECK(out.value == 1);
    CHECK(Tracked::live == 3);  // 2 queued + `out`
    // Wrap a few times with live objects.
    for (std::uint64_t i = 0; i < 20; ++i) {
      CHECK(q.try_emplace(100 + i));
      CHECK(q.try_pop(out));
    }
    CHECK(Tracked::live == 3);
  }
  // Destructor destroyed the two elements that were still queued.
  CHECK(Tracked::live == 0);
}

void move_only_and_non_trivial_types() {
  SpscQueue<std::unique_ptr<std::string>> q(2);
  CHECK(q.try_push(std::make_unique<std::string>("alpha")));
  CHECK(q.try_emplace(new std::string("beta")));
  CHECK(!q.try_push(std::make_unique<std::string>("gamma")));
  auto a = q.try_pop();
  CHECK(a.has_value() && **a == "alpha");
  std::unique_ptr<std::string> b;
  CHECK(q.try_pop(b) && *b == "beta");
  CHECK(q.try_push(std::make_unique<std::string>("left in queue")));  // freed by ~SpscQueue
}

void cursors_are_isolated() {
  // The padded queue keeps shared-immutable, producer and consumer state on
  // separate cache lines; the benchmark-only unpadded variant does not.
  static_assert(alignof(SpscQueue<int>) == lfm::kCacheLine);
  static_assert(sizeof(SpscQueue<int>) == 3 * lfm::kCacheLine);
  static_assert(sizeof(SpscQueue<int, 8>) < lfm::kCacheLine);
}

}  // namespace

int main() {
  std::printf("spsc_basic_test\n");
  RUN(rejects_bad_capacity);
  RUN(empty_pop_fails);
  RUN(full_push_fails_without_overwrite);
  RUN(capacity_one);
  RUN(wraparound_preserves_order);
  RUN(batch_push_pop);
  RUN(consume_in_place);
  RUN(consume_handles_throwing_callback);
  RUN(lifetime_is_balanced);
  RUN(move_only_and_non_trivial_types);
  RUN(cursors_are_isolated);
  std::printf("OK\n");
  return 0;
}
