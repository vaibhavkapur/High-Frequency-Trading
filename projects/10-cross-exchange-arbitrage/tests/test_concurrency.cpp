#include <atomic>
#include <thread>
#include <vector>

#include "test_harness.hpp"
#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/spsc_queue.hpp"
#include "xarb/synth_feed.hpp"
#include "xarb/venue_adapter.hpp"

using namespace xarb;

// Exercises 3 concurrent venue-ingestion producer threads feeding 1 single-threaded
// market-data consumer loop via lock-free SPSC ring buffers (validated under TSan).
XARB_TEST(MultiVenueSpscIngestionPipeline) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  SynthFeedConfig scfg;
  scfg.num_steps = 1500;
  scfg.seed = 314159ULL;
  scfg.inject_faults = false;
  auto full_feed = generate_synthetic_feed(reg, scfg);

  std::array<std::vector<RecordedFeedEvent>, kVenueCount> per_venue;
  for (auto& ev : full_feed) {
    per_venue[static_cast<size_t>(ev.venue)].push_back(std::move(ev));
  }

  std::array<SpscQueue<RecordedFeedEvent>, kVenueCount> queues{
      SpscQueue<RecordedFeedEvent>(256),
      SpscQueue<RecordedFeedEvent>(256),
      SpscQueue<RecordedFeedEvent>(256),
  };
  std::array<std::atomic<bool>, kVenueCount> producers_done{false, false, false};

  std::vector<std::thread> producers;
  for (size_t v = 0; v < kVenueCount; ++v) {
    producers.emplace_back([&, v]() {
      for (const auto& ev : per_venue[v]) {
        while (!queues[v].try_push(ev)) {
          std::this_thread::yield();
        }
      }
      producers_done[v].store(true, std::memory_order_release);
    });
  }

  const auto& btc = *reg.get(0);
  std::array<std::unique_ptr<VenueAdapter>, kVenueCount> adapters;
  for (size_t v = 0; v < kVenueCount; ++v) {
    adapters[v] = make_venue_adapter(static_cast<VenueId>(v), btc, 2000LL * kNsPerMs);
  }

  std::array<size_t, kVenueCount> consumed{0, 0, 0};
  while (true) {
    bool did_work = false;
    bool all_done = true;
    for (size_t v = 0; v < kVenueCount; ++v) {
      RecordedFeedEvent ev;
      while (queues[v].try_pop(ev)) {
        did_work = true;
        consumed[v] += 1;
        if (ev.kind == FeedEventKind::WsMessage) {
          adapters[v]->on_ws_message(ev.payload, ev.recv_ns);
        } else if (ev.kind == FeedEventKind::RestSnapshot) {
          adapters[v]->on_rest_snapshot(ev.payload, ev.recv_ns);
        } else if (ev.kind == FeedEventKind::Reconnect) {
          adapters[v]->on_reconnect(ev.recv_ns);
        }
      }
      if (!producers_done[v].load(std::memory_order_acquire) || !queues[v].empty()) {
        all_done = false;
      }
    }
    if (all_done) break;
    if (!did_work) std::this_thread::yield();
  }

  for (auto& t : producers) t.join();

  for (size_t v = 0; v < kVenueCount; ++v) {
    EXPECT_EQ(consumed[v], per_venue[v].size());
    EXPECT_EQ(adapters[v]->state(), BookState::Synced);
    EXPECT_EQ(adapters[v]->metrics().gaps_detected, 0ULL);
    EXPECT_EQ(adapters[v]->metrics().checksum_mismatches, 0ULL);
    EXPECT_EQ(adapters[v]->metrics().parse_errors, 0ULL);
  }
}

int main() { return xarb::test::run_all("test_concurrency"); }
