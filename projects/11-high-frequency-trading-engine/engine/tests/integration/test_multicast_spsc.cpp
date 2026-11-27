#include <atomic>
#include <thread>

#include "hft/adapters/backtester_adapter.hpp"
#include "hft/adapters/multicast_adapter.hpp"
#include "hft/adapters/spsc_adapter.hpp"
#include "hft/runtime/canonical_book.hpp"
#include "hft/runtime/pipeline.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

HFT_TEST(MdfMulticastConvergesUnderDropsCorruptionDuplicatesAndReordering) {
  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);

  MulticastFeedAdapter::Config cfg{};
  cfg.venue = 0;
  cfg.channel_id = 1;
  cfg.retransmit_history_packets = 64;
  cfg.checkpoint_every_packets = 4;
  cfg.max_gap_before_snapshot = 4;
  cfg.fault_config.seed = 42;
  cfg.fault_config.drop_prob = 0.20;
  cfg.fault_config.duplicate_prob = 0.15;
  cfg.fault_config.corrupt_prob = 0.10;
  cfg.fault_config.reorder_prob = 0.20;
  cfg.fault_config.max_reorder_delay = 2;

  MulticastFeedAdapter mdf_ad(cfg);
  std::uint64_t t = 1'000'000ULL;
  for (std::size_t i = 0; i < feed.size(); i += 4) {
    const std::size_t n = std::min<std::size_t>(4, feed.size() - i);
    mdf_ad.publish_batch(std::span<const Envelope>(feed.data() + i, n), t);
    t += 50'000ULL;
  }
  mdf_ad.flush_and_recover(t);

  const auto sum = mdf_ad.summary();
  HFT_EXPECT_TRUE(sum.packets_published > 10);
  HFT_EXPECT_TRUE(sum.gaps_detected > 0);
  HFT_EXPECT_TRUE(sum.retransmit_packets_recovered > 0 || sum.snapshots_applied > 0);
  HFT_EXPECT_TRUE(sum.synced);
  HFT_EXPECT_TRUE(sum.state_converged);
  HFT_EXPECT_EQ(sum.publisher_state_hash, sum.subscriber_state_hash);
}

HFT_TEST(MdfLargeGapTriggersSnapshotFallbackAndBlocksTradingWhileOutOfSync) {
  const auto feed = BacktesterFeedAdapter::generate_l3_envelopes(0, 300, 1, 7, 10'000);

  MulticastFeedAdapter::Config cfg{};
  cfg.venue = 0;
  cfg.channel_id = 1;
  cfg.retransmit_history_packets = 8;  // small history forces snapshot fallback!
  cfg.checkpoint_every_packets = 5;
  cfg.max_gap_before_snapshot = 3;
  cfg.fault_config.drop_ranges = {{5, 18}};  // drop 14 consecutive packets -> exceeds history

  MulticastFeedAdapter mdf_ad(cfg);
  // Temporarily make TCP recovery unavailable during the outage window so we verify the
  // receiver enters out-of-sync state and blocks strategy trading until recovery is restored!
  mdf_ad.set_recovery_available(false);
  bool observed_out_of_sync = false;
  CanonicalOrderBook guard_book;

  std::uint64_t t = 1'000'000ULL;
  for (std::size_t i = 0; i < feed.size(); i += 4) {
    const std::size_t n = std::min<std::size_t>(4, feed.size() - i);
    mdf_ad.publish_batch(std::span<const Envelope>(feed.data() + i, n), t);
    t += 50'000ULL;
    if (!mdf_ad.is_synced()) {
      observed_out_of_sync = true;
      guard_book.set_health(mdf_ad.feed_health());
      HFT_EXPECT_TRUE(!guard_book.is_tradable(SimTimeNs{static_cast<std::int64_t>(t)}));
    }
  }
  HFT_EXPECT_TRUE(observed_out_of_sync);

  mdf_ad.set_recovery_available(true);
  mdf_ad.flush_and_recover(t);
  const auto sum = mdf_ad.summary();
  HFT_EXPECT_TRUE(sum.snapshots_applied > 0);
  HFT_EXPECT_TRUE(sum.synced);
  HFT_EXPECT_TRUE(sum.state_converged);
  HFT_EXPECT_EQ(sum.publisher_state_hash, sum.subscriber_state_hash);
}

HFT_TEST(SpscQueueOverflowPoliciesNeverSilentlyDropOrdersOrExecs) {
  // 1. Reject policy returns QueueFull explicitly when capacity (4) is reached.
  SpscTransportAdapter q_rej(4, QueueOverflowPolicy::Reject);
  Envelope md_env{};
  md_env.payload = MarketDataEvent{};
  for (int i = 0; i < 4; ++i) {
    HFT_EXPECT_EQ(q_rej.push(md_env), ErrorCode::Ok);
  }
  HFT_EXPECT_EQ(q_rej.push(md_env), ErrorCode::QueueFull);
  HFT_EXPECT_EQ(q_rej.stats().rejected, 1ULL);

  // 2. DropAndResync drops ONLY MarketDataEvent (raising resync flag), NEVER OrderIntent or ExecReport!
  SpscTransportAdapter q_drop(4, QueueOverflowPolicy::DropAndResync);
  for (int i = 0; i < 4; ++i) {
    HFT_EXPECT_EQ(q_drop.push(md_env), ErrorCode::Ok);
  }
  HFT_EXPECT_EQ(q_drop.push(md_env), ErrorCode::FeedGap);
  HFT_EXPECT_TRUE(q_drop.resync_required());
  HFT_EXPECT_EQ(q_drop.stats().dropped_for_resync, 1ULL);

  Envelope ord_env{};
  ord_env.payload = OrderIntent{};
  Envelope exec_env{};
  exec_env.payload = ExecutionReport{};
  HFT_EXPECT_EQ(q_drop.push(ord_env), ErrorCode::QueueFull);
  HFT_EXPECT_EQ(q_drop.push(exec_env), ErrorCode::QueueFull);
  HFT_EXPECT_EQ(q_drop.stats().dropped_for_resync, 1ULL);  // unchanged!
  HFT_EXPECT_EQ(q_drop.stats().rejected, 2ULL);

  // 3. Two-thread Backpressure run preserves every envelope & exact book hash.
  const auto feed = BacktesterFeedAdapter::generate_l3_envelopes(0, 1500, 1, 99, 10'000);
  CanonicalOrderBook direct_book;
  for (const auto& e : feed) {
    direct_book.apply(std::get<MarketDataEvent>(e.payload), e.event_time_ns);
  }

  SpscTransportAdapter q_bp(64, QueueOverflowPolicy::Backpressure, 500'000);
  std::atomic<bool> done{false};
  std::uint64_t threaded_hash = 0;
  std::uint64_t count = 0;

  std::thread consumer([&]() {
    CanonicalOrderBook book;
    Envelope e{};
    while (true) {
      if (q_bp.try_pop(e)) {
        book.apply(std::get<MarketDataEvent>(e.payload), e.event_time_ns);
        ++count;
      } else if (done.load(std::memory_order_acquire)) {
        while (q_bp.try_pop(e)) {
          book.apply(std::get<MarketDataEvent>(e.payload), e.event_time_ns);
          ++count;
        }
        break;
      } else {
        lfm::cpu_relax();
      }
    }
    threaded_hash = book.state_hash();
  });

  for (const auto& e : feed) {
    HFT_EXPECT_EQ(q_bp.push(e), ErrorCode::Ok);
  }
  done.store(true, std::memory_order_release);
  consumer.join();

  HFT_EXPECT_EQ(count, feed.size());
  HFT_EXPECT_EQ(threaded_hash, direct_book.state_hash());
  HFT_EXPECT_EQ(q_bp.stats().dropped_for_resync, 0ULL);
}

int main() { return hft::test::run_all("test_multicast_spsc"); }
