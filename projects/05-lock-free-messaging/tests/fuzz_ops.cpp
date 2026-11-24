// Differential / property test driven by raw bytes.
//
// Each input is decoded into a queue capacity and a sequence of operations.
// The operations are applied (single-threaded) to SpscQueue, MpscQueue,
// MutexQueue and a trivially-correct std::deque model, and every observable
// result must agree. A second SpscQueue of instance-counting elements checks
// that construction and destruction stay balanced.
//
// Two ways to run:
//   * default: a standalone driver feeds deterministic pseudo-random inputs
//     (this is the CTest target `fuzz_ops`)
//   * -DLFM_LIBFUZZER=ON: built as a libFuzzer target for coverage-guided
//     fuzzing (needs a clang that ships libFuzzer; Apple clang does not)

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "lfm/mpsc_queue.hpp"
#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"
#include "test_util.hpp"

using testutil::Tracked;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::size_t capacity = std::size_t{1} << (data[0] % 6);  // 1..32

  CHECK(Tracked::live == 0);
  {
    lfm::SpscQueue<std::uint64_t> spsc(capacity);
    lfm::SpscQueue<Tracked> tracked(capacity);
    // MpscQueue needs capacity >= 2; it sits out capacity-1 inputs.
    const bool with_mpsc = capacity >= 2;
    lfm::MpscQueue<std::uint64_t> mpsc(with_mpsc ? capacity : 2);
    const auto mpsc_push = [&](std::uint64_t v, bool expect) {
      if (with_mpsc) {
        CHECK(mpsc.try_push(v) == expect);
      }
    };
    const auto mpsc_pop = [&](std::uint64_t want) {
      if (with_mpsc) {
        std::uint64_t got = 0;
        CHECK(mpsc.try_pop(got));
        CHECK(got == want);
      }
    };
    lfm::MutexQueue<std::uint64_t> mutex(capacity);
    std::deque<std::uint64_t> model;

    std::uint64_t next = 1;
    std::array<std::uint64_t, 16> buf{};
    std::array<Tracked, 16> tbuf{};

    std::size_t i = 1;
    const auto arg = [&]() -> std::size_t { return i < size ? data[i++] % 17U : 0U; };

    while (i < size) {
      switch (data[i++] % 6) {
        case 0: {  // push one
          const bool expect = model.size() < capacity;
          CHECK(spsc.try_push(next) == expect);
          CHECK(tracked.try_emplace(next) == expect);
          mpsc_push(next, expect);
          CHECK(mutex.try_push(next) == expect);
          if (expect) {
            model.push_back(next);
          }
          ++next;
          break;
        }
        case 1: {  // pop one
          const bool expect = !model.empty();
          std::uint64_t a = 0;
          std::uint64_t c = 0;
          Tracked t;
          CHECK(spsc.try_pop(a) == expect);
          CHECK(tracked.try_pop(t) == expect);
          CHECK(mutex.try_pop(c) == expect);
          if (expect) {
            const std::uint64_t want = model.front();
            model.pop_front();
            CHECK(a == want && c == want && t.value == want);
            mpsc_pop(want);
          } else if (with_mpsc) {
            CHECK(!mpsc.try_pop(a));
          }
          break;
        }
        case 2: {  // push batch of k
          const std::size_t k = arg() % (buf.size() + 1);
          for (std::size_t j = 0; j < k; ++j) {
            buf[j] = next + j;
            tbuf[j].value = next + j;
          }
          const std::size_t expect = std::min(k, capacity - model.size());
          CHECK(spsc.try_push_batch(std::span<const std::uint64_t>(buf.data(), k)) == expect);
          CHECK(tracked.try_push_batch(std::span<const Tracked>(tbuf.data(), k)) == expect);
          for (std::size_t j = 0; j < k; ++j) {
            mpsc_push(buf[j], j < expect);
            CHECK(mutex.try_push(buf[j]) == (j < expect));
          }
          for (std::size_t j = 0; j < expect; ++j) {
            model.push_back(buf[j]);
          }
          next += k;
          break;
        }
        case 3: {  // pop batch of up to k
          const std::size_t k = arg() % (buf.size() + 1);
          const std::size_t expect = std::min(k, model.size());
          CHECK(spsc.try_pop_batch(std::span<std::uint64_t>(buf.data(), k)) == expect);
          CHECK(tracked.try_pop_batch(std::span<Tracked>(tbuf.data(), k)) == expect);
          for (std::size_t j = 0; j < expect; ++j) {
            std::uint64_t c = 0;
            CHECK(mutex.try_pop(c));
            const std::uint64_t want = model.front();
            model.pop_front();
            CHECK(buf[j] == want && tbuf[j].value == want && c == want);
            mpsc_pop(want);
          }
          break;
        }
        case 4: {  // consume up to k in place
          const std::size_t k = arg();
          const std::size_t expect = std::min(k, model.size());
          std::vector<std::uint64_t> got;
          std::vector<std::uint64_t> tgot;
          CHECK(spsc.consume([&](std::uint64_t&& v) { got.push_back(v); }, k) == expect);
          CHECK(tracked.consume([&](Tracked&& t) { tgot.push_back(t.value); }, k) == expect);
          for (std::size_t j = 0; j < expect; ++j) {
            std::uint64_t c = 0;
            CHECK(mutex.try_pop(c));
            const std::uint64_t want = model.front();
            model.pop_front();
            CHECK(got[j] == want && tgot[j] == want && c == want);
            mpsc_pop(want);
          }
          break;
        }
        default:  // observers
          CHECK(spsc.size_approx() == model.size());
          CHECK(tracked.size_approx() == model.size());
          CHECK(!with_mpsc || mpsc.size_approx() == model.size());
          CHECK(mutex.size_approx() == model.size());
          CHECK(spsc.empty_approx() == model.empty());
          break;
      }
      // Live Tracked objects: those queued plus the 16 in the scratch buffer.
      CHECK(Tracked::live == static_cast<std::int64_t>(model.size() + tbuf.size()));
    }
  }
  CHECK(Tracked::live == 0);  // queue destructors released everything left
  return 0;
}

#if !defined(LFM_LIBFUZZER)
int main() {
  const std::uint64_t inputs = testutil::stress_iters(20'000);
  std::printf("fuzz_ops (standalone driver, %llu inputs)\n",
              static_cast<unsigned long long>(inputs));
  testutil::Rng rng{0x5eed};
  std::vector<std::uint8_t> input;
  for (std::uint64_t n = 0; n < inputs; ++n) {
    input.resize(1 + rng.next() % 512);
    // Bias the op mix per input (push-heavy, pop-heavy, balanced) so both the
    // full and the empty boundary are hit often.
    const std::uint64_t bias = rng.next() % 3;
    for (auto& b : input) {
      std::uint64_t r = rng.next();
      if (bias == 0 && r % 4 == 0) {
        r = (r >> 8) & ~std::uint64_t{0xff};  // low byte 0: push
      } else if (bias == 1 && r % 4 == 0) {
        r = 1 + 6 * ((r >> 8) % 40);  // == 1 mod 6: pop
      }
      b = static_cast<std::uint8_t>(r);
    }
    LLVMFuzzerTestOneInput(input.data(), input.size());
  }
  std::printf("OK\n");
  return 0;
}
#endif
