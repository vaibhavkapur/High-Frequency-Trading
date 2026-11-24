// End-to-end example: a feed thread publishes synthetic quotes to a strategy
// thread over an SpscQueue, twice, to show the two overload policies:
//
//   1. backpressure - the feed waits when the strategy falls behind; nothing
//      is lost, the feed is slowed down
//   2. drop         - the feed never waits; quotes that do not fit are dropped
//      and counted
//
// The data is synthetic (a deterministic random walk). Nothing here connects
// to a real venue.
//
//   market_data_pipeline [--quotes N] [--capacity N] [--slow-every N]

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "lfm/endpoints.hpp"
#include "lfm/spsc_queue.hpp"

namespace {

struct Quote {
  std::uint64_t seq;
  std::uint32_t symbol;
  std::uint32_t size;
  std::int64_t price_ticks;
};
static_assert(std::is_trivially_copyable_v<Quote>);

constexpr std::uint32_t kSymbols = 4;
constexpr std::array<const char*, kSymbols> kNames{"AAA", "BBB", "CCC", "DDD"};

struct SymbolStats {
  std::uint64_t quotes = 0;
  std::uint64_t volume = 0;
  double notional = 0;
  std::int64_t last = 0;
};

using Queue = lfm::SpscQueue<Quote>;

struct Options {
  std::uint64_t quotes = 2'000'000;
  std::size_t capacity = 1024;
  std::uint64_t slow_every = 64;  // strategy does extra work every Nth quote
};

Quote next_quote(std::uint64_t seq, std::uint64_t& rng, std::array<std::int64_t, kSymbols>& px) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  const auto symbol = static_cast<std::uint32_t>(rng % kSymbols);
  px[symbol] += static_cast<std::int64_t>((rng >> 8) % 3) - 1;  // -1, 0 or +1 tick
  return {seq, symbol, 100 * (1 + static_cast<std::uint32_t>((rng >> 16) % 5)), px[symbol]};
}

void run(const char* title, bool drop_when_full, const Options& opt) {
  Queue queue(opt.capacity);
  lfm::Producer<Queue> feed(queue);
  lfm::Consumer<Queue> strategy(queue);
  std::atomic<bool> feed_done{false};

  std::thread feed_thread([&] {
    std::uint64_t rng = 0x9e3779b97f4a7c15ULL;
    std::array<std::int64_t, kSymbols> px{10'000, 20'000, 30'000, 40'000};
    for (std::uint64_t seq = 0; seq < opt.quotes; ++seq) {
      const Quote q = next_quote(seq, rng, px);
      if (drop_when_full) {
        feed.push_or_drop(q);
      } else {
        feed.push(q);
      }
    }
    feed_done.store(true, std::memory_order_release);
  });

  std::array<SymbolStats, kSymbols> stats{};
  std::uint64_t gaps = 0;
  std::uint64_t expected_seq = 0;
  bool ordered = true;
  volatile std::uint64_t sink = 0;
  Quote q{};
  // Drain until the feed has finished AND the queue is empty. The flag is
  // re-checked after each failed pop, and it is set after the last push, so
  // no quote can be missed.
  while (strategy.pop(q, [&] { return feed_done.load(std::memory_order_acquire); }) ||
         strategy.try_pop(q)) {
    ordered &= q.seq >= expected_seq;  // never goes backwards, even when dropping
    gaps += q.seq - expected_seq;
    expected_seq = q.seq + 1;
    SymbolStats& s = stats[q.symbol];
    ++s.quotes;
    s.volume += q.size;
    s.notional += static_cast<double>(q.price_ticks) * q.size;
    s.last = q.price_ticks;
    if (opt.slow_every != 0 && q.seq % opt.slow_every == 0) {
      for (std::uint64_t i = 0; i < 2000; ++i) {  // simulated slow path
        sink = sink + i;
      }
    }
  }
  feed_thread.join();
  gaps += opt.quotes - expected_seq;  // quotes dropped after the last one received

  const lfm::ProducerStats ps = feed.stats();
  const lfm::ConsumerStats cs = strategy.stats();
  std::printf("\n== %s ==\n", title);
  std::printf(
      "feed:     offered=%llu pushed=%llu dropped=%llu full_events=%llu wait_spins=%llu\n",
      static_cast<unsigned long long>(opt.quotes), static_cast<unsigned long long>(ps.pushed),
      static_cast<unsigned long long>(ps.dropped), static_cast<unsigned long long>(ps.full_events),
      static_cast<unsigned long long>(ps.wait_spins));
  std::printf("strategy: received=%llu sequence_gaps=%llu empty_events=%llu in_order=%s\n",
              static_cast<unsigned long long>(cs.popped), static_cast<unsigned long long>(gaps),
              static_cast<unsigned long long>(cs.empty_events), ordered ? "yes" : "NO");
  for (std::uint32_t i = 0; i < kSymbols; ++i) {
    const SymbolStats& s = stats[i];
    std::printf("  %s quotes=%-8llu volume=%-10llu vwap=%.2f last=%lld\n", kNames[i],
                static_cast<unsigned long long>(s.quotes),
                static_cast<unsigned long long>(s.volume),
                s.volume == 0 ? 0.0 : s.notional / static_cast<double>(s.volume),
                static_cast<long long>(s.last));
  }
  // Accounting must balance exactly in both modes.
  const bool balanced = ps.pushed == cs.popped && ps.pushed + ps.dropped == opt.quotes &&
                        gaps == ps.dropped && ordered;
  std::printf("accounting %s: pushed == received, pushed + dropped == offered, gaps == dropped\n",
              balanced ? "OK" : "MISMATCH");
  if (!balanced) {
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::uint64_t value = std::strtoull(argv[i + 1], nullptr, 10);
    if (std::strcmp(argv[i], "--quotes") == 0) {
      opt.quotes = value;
    } else if (std::strcmp(argv[i], "--capacity") == 0) {
      opt.capacity = value;
    } else if (std::strcmp(argv[i], "--slow-every") == 0) {
      opt.slow_every = value;
    } else {
      std::fprintf(stderr, "usage: %s [--quotes N] [--capacity N] [--slow-every N]\n", argv[0]);
      return 2;
    }
  }
  std::printf("synthetic quotes=%llu, queue capacity=%zu, slow path every %llu quotes\n",
              static_cast<unsigned long long>(opt.quotes), opt.capacity,
              static_cast<unsigned long long>(opt.slow_every));
  run("policy: backpressure (lossless, feed waits)", false, opt);
  run("policy: drop on full (feed never waits)", true, opt);
  return 0;
}
