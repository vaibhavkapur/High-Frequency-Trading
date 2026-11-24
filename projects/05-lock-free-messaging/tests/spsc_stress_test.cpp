// Two-thread stress tests for SpscQueue. One producer sends monotonically
// numbered messages; one consumer checks it sees exactly 0,1,2,... with no
// loss, duplication or reordering. Run under ThreadSanitizer via
// scripts/run_sanitizers.sh.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"
#include "test_util.hpp"

namespace {

// Padded so a torn or stale read would show up as mismatched fields.
struct Message {
  std::uint64_t seq;
  std::uint64_t check;  // always ~seq
  std::array<std::uint64_t, 4> fill;
};

Message make(std::uint64_t seq) { return {seq, ~seq, {seq, seq + 1, seq + 2, seq + 3}}; }

bool valid(const Message& m, std::uint64_t seq) {
  return m.seq == seq && m.check == ~seq && m.fill[0] == seq && m.fill[3] == seq + 3;
}

// Yielding on failure keeps the test meaningful on machines with fewer cores
// than threads (CI runners) and maximises interleavings under TSan.
void single_item(std::size_t capacity, std::uint64_t n) {
  lfm::SpscQueue<Message> q(capacity);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!q.try_push(make(i))) {
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t failures = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    Message m{};
    while (!q.try_pop(m)) {
      std::this_thread::yield();
    }
    failures += valid(m, i) ? 0 : 1;
  }
  producer.join();
  CHECK(failures == 0);
  CHECK(q.empty_approx());
}

void batched(std::size_t capacity, std::uint64_t n) {
  lfm::SpscQueue<Message> q(capacity);
  std::thread producer([&] {
    std::array<Message, 13> batch{};
    std::uint64_t next = 0;
    while (next < n) {
      const std::size_t want =
          static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(), n - next));
      for (std::size_t i = 0; i < want; ++i) {
        batch[i] = make(next + i);
      }
      std::size_t sent = 0;
      while (sent < want) {
        const std::size_t k =
            q.try_push_batch(std::span<const Message>(batch).subspan(sent, want - sent));
        if (k == 0) {
          std::this_thread::yield();
        }
        sent += k;
      }
      next += want;
    }
  });
  std::uint64_t expect = 0;
  std::uint64_t failures = 0;
  std::array<Message, 7> out{};
  bool use_consume = false;
  while (expect < n) {
    std::size_t got = 0;
    if (use_consume) {
      got = q.consume([&](Message&& m) { failures += valid(m, expect++) ? 0 : 1; });
    } else {
      got = q.try_pop_batch(out);
      for (std::size_t i = 0; i < got; ++i) {
        failures += valid(out[i], expect++) ? 0 : 1;
      }
    }
    if (got == 0) {
      std::this_thread::yield();
    }
    use_consume = !use_consume;
  }
  producer.join();
  CHECK(failures == 0);
  CHECK(expect == n);
  CHECK(q.empty_approx());
}

// Non-trivial payload: heap-owning strings cross the queue; ASan/LSan would
// flag a leak or double free.
void non_trivial_payload(std::uint64_t n) {
  lfm::SpscQueue<std::string> q(64);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      std::string s = "message-number-" + std::to_string(i) + "-with-padding-beyond-sso";
      while (!q.try_push(std::move(s))) {  // not moved-from on failure
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t failures = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::string s;
    while (!q.try_pop(s)) {
      std::this_thread::yield();
    }
    failures += s == "message-number-" + std::to_string(i) + "-with-padding-beyond-sso" ? 0 : 1;
  }
  producer.join();
  CHECK(failures == 0);
}

// "Compare against a reference queue under the same message mix": push the
// same pseudo-random stream through the SPSC queue and the mutex reference
// queue and require identical received streams (order-sensitive digest).
template <typename Q>
std::uint64_t digest_through(Q& q, std::uint64_t n, std::uint64_t seed) {
  std::thread producer([&] {
    testutil::Rng rng{seed};
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t v = rng.next();
      while (!q.try_push(v)) {
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t digest = 0xcbf29ce484222325ULL;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t v = 0;
    while (!q.try_pop(v)) {
      std::this_thread::yield();
    }
    digest = (digest ^ v) * 0x100000001b3ULL;
  }
  producer.join();
  return digest;
}

void matches_reference_queue(std::uint64_t n) {
  lfm::SpscQueue<std::uint64_t> spsc(256);
  lfm::MutexQueue<std::uint64_t> reference(256);
  testutil::Rng rng{7};
  std::uint64_t expected = 0xcbf29ce484222325ULL;
  for (std::uint64_t i = 0; i < n; ++i) {
    expected = (expected ^ rng.next()) * 0x100000001b3ULL;
  }
  CHECK(digest_through(spsc, n, 7) == expected);
  CHECK(digest_through(reference, n, 7) == expected);
}

}  // namespace

int main() {
  const std::uint64_t n = testutil::stress_iters(2'000'000);
  std::printf("spsc_stress_test (%llu messages per case)\n", static_cast<unsigned long long>(n));

  // Tiny capacities force constant full/empty transitions and wraparound;
  // the large one exercises the cached-cursor fast path.
  for (std::size_t cap : {std::size_t{1}, std::size_t{2}, std::size_t{16}, std::size_t{4096}}) {
    std::printf("  single_item capacity=%zu\n", cap);
    single_item(cap, n);
  }
  for (std::size_t cap : {std::size_t{4}, std::size_t{32}, std::size_t{1024}}) {
    std::printf("  batched capacity=%zu\n", cap);
    batched(cap, n);
  }
  std::printf("  non_trivial_payload\n");
  non_trivial_payload(n / 10);
  std::printf("  matches_reference_queue\n");
  matches_reference_queue(n / 4);
  std::printf("OK\n");
  return 0;
}
