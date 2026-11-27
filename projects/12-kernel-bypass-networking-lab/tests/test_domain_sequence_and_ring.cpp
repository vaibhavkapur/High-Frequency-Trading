#include "test_framework.hpp"

#include "kbnl/benchmark_runner.hpp"
#include "kbnl/latency_stats.hpp"
#include "kbnl/pacer.hpp"
#include "kbnl/ring_buffer.hpp"
#include "kbnl/sequence_tracker.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

kbnl::PacketHeader make_hdr(uint64_t seq, uint16_t flags = kbnl::kFlagNone) {
    kbnl::PacketHeader h{};
    h.seq_num = seq;
    h.flags = flags;
    return h;
}

KBNL_TEST(sequence_tracker_loss_ooo_duplicates_and_invariant) {
    kbnl::SequenceTracker tracker;

    // Receive: 1, 2, [drop 3,4], 5, 6, 4(ooo), 5(dup), [drop 7], 8, EOS
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(1, kbnl::kFlagWarmup))),
                   static_cast<int>(kbnl::PacketObservation::kInOrder));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(2))),
                   static_cast<int>(kbnl::PacketObservation::kInOrder));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(5))),
                   static_cast<int>(kbnl::PacketObservation::kGapDetected));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(6))),
                   static_cast<int>(kbnl::PacketObservation::kInOrder));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(4))),
                   static_cast<int>(kbnl::PacketObservation::kOutOfOrder));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(5))),
                   static_cast<int>(kbnl::PacketObservation::kDuplicate));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(8))),
                   static_cast<int>(kbnl::PacketObservation::kGapDetected));
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(0, kbnl::kFlagEndOfStream))),
                   static_cast<int>(kbnl::PacketObservation::kEndOfStream));

    const auto& c = tracker.counters();
    KBNL_EXPECT_TRUE(c.accounting_invariant_holds());
    KBNL_EXPECT_EQ(c.unique_received, 6ULL);
    KBNL_EXPECT_EQ(c.warmup_received, 1ULL);
    KBNL_EXPECT_EQ(c.measured_received, 5ULL);
    KBNL_EXPECT_EQ(c.missing_within_highest, 2ULL); // seq 3 and seq 7
    KBNL_EXPECT_EQ(c.out_of_order, 1ULL);
    KBNL_EXPECT_EQ(c.duplicates, 1ULL);
    KBNL_EXPECT_EQ(c.gaps_events, 2ULL);
    KBNL_EXPECT_EQ(c.eos_packets, 1ULL);
    KBNL_EXPECT_EQ(c.lost_against_expected(10), 4ULL); // 3, 7, 9, 10
}

KBNL_TEST(sequence_tracker_initial_gap_and_too_old_window_eviction) {
    kbnl::SequenceTracker tracker;
    // First packet is seq 4 (1, 2, 3 missed initially)
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(4))),
                   static_cast<int>(kbnl::PacketObservation::kGapDetected));
    KBNL_EXPECT_EQ(tracker.counters().missing_within_highest, 3ULL);
    KBNL_EXPECT_TRUE(tracker.counters().accounting_invariant_holds());

    // Jump beyond reorder window (> 1024 ahead)
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(2000))),
                   static_cast<int>(kbnl::PacketObservation::kGapDetected));
    // Now seq 2 arrives, which is older than kReorderWindowSize (1024)
    KBNL_EXPECT_EQ(static_cast<int>(tracker.on_packet(make_hdr(2))),
                   static_cast<int>(kbnl::PacketObservation::kTooOldOutOfWindow));
    KBNL_EXPECT_EQ(tracker.counters().too_old_out_of_window, 1ULL);
    KBNL_EXPECT_TRUE(tracker.counters().accounting_invariant_holds());
}

KBNL_TEST(burst_pacer_schedule_and_burst_accounting) {
    kbnl::WorkloadConfig w{};
    w.pacing_mode = kbnl::PacingMode::kBurst;
    w.burst_size = 16;
    w.burst_interval_us = 1000; // 1 ms
    w.warmup_packets = 16;
    w.measured_packets = 64;

    kbnl::PacerSchedule sched(w);
    KBNL_EXPECT_EQ(sched.total_data_packets(), 80ULL);
    // Packets 0..15 all scheduled at offset 0 (first burst)
    for (uint64_t i = 0; i < 16; ++i) {
        KBNL_EXPECT_EQ(sched.scheduled_offset_ns(i), 0ULL);
        KBNL_EXPECT_TRUE(sched.is_warmup(i));
        KBNL_EXPECT_TRUE(sched.is_burst_packet(i));
    }
    // Packets 16..31 scheduled at 1,000,000 ns (second burst)
    for (uint64_t i = 16; i < 32; ++i) {
        KBNL_EXPECT_EQ(sched.scheduled_offset_ns(i), 1000000ULL);
        KBNL_EXPECT_FALSE(sched.is_warmup(i));
    }
    // Packet 64 -> burst 4 -> 4,000,000 ns
    KBNL_EXPECT_EQ(sched.scheduled_offset_ns(64), 4000000ULL);
}

KBNL_TEST(spsc_ring_buffer_queue_saturation_and_fifo_ordering) {
    // Reject non-power-of-two capacity
    bool threw = false;
    try {
        kbnl::SpscRingBuffer<uint64_t> bad_ring(10);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    KBNL_EXPECT_TRUE(threw);

    // Bounded capacity 8: push 12 items -> 8 succeed, 4 drop deterministically
    kbnl::SpscRingBuffer<uint64_t> ring(8);
    kbnl::SequenceTracker tracker;
    for (uint64_t i = 1; i <= 12; ++i) {
        if (!ring.try_push(i)) {
            tracker.on_ring_drop(1);
        }
    }
    KBNL_EXPECT_EQ(ring.size_approx(), size_t{8});
    KBNL_EXPECT_EQ(ring.dropped_count(), 4ULL);
    KBNL_EXPECT_EQ(tracker.counters().ring_drops, 4ULL);

    // Drain in exact FIFO order 1..8
    for (uint64_t expected = 1; expected <= 8; ++expected) {
        uint64_t val = 0;
        KBNL_EXPECT_TRUE(ring.try_pop(val));
        KBNL_EXPECT_EQ(val, expected);
    }
    uint64_t dummy = 0;
    KBNL_EXPECT_FALSE(ring.try_pop(dummy));

    // Verify wrap-around under sustained push/pop cycles
    for (uint64_t round = 0; round < 50; ++round) {
        KBNL_EXPECT_TRUE(ring.try_push(round));
        uint64_t out = 0;
        KBNL_EXPECT_TRUE(ring.try_pop(out));
        KBNL_EXPECT_EQ(out, round);
    }
}

KBNL_TEST(latency_stats_exact_quantiles_and_warmup_exclusion) {
    kbnl::LatencyStats stats(1000);
    // Warmup samples should not affect measured stats
    stats.record_sample(1000, 999999, true);
    stats.record_sample(2000, 888888, true);

    // Record 100 measured samples: 100, 200, ..., 10000 ns
    for (uint64_t i = 1; i <= 100; ++i) {
        stats.record_sample(10000, 10000 + i * 100, false);
    }
    const auto s = stats.summarize();
    KBNL_EXPECT_EQ(s.count, 100ULL);
    KBNL_EXPECT_EQ(s.warmup_ignored, 2ULL);
    KBNL_EXPECT_TRUE(s.exact_quantiles);
    KBNL_EXPECT_EQ(s.min_ns, 100ULL);
    KBNL_EXPECT_EQ(s.max_ns, 10000ULL);
    KBNL_EXPECT_NEAR(s.mean_ns, 5050.0, 0.01);
    KBNL_EXPECT_EQ(s.p50_ns, 5100ULL);
    KBNL_EXPECT_EQ(s.p99_ns, 10000ULL);
    KBNL_EXPECT_EQ(s.p999_ns, 10000ULL);
}

KBNL_TEST(latency_stats_bucket_fallback_and_negative_clamp) {
    // Force bucket fallback by setting max_exact_samples = 4
    kbnl::LatencyStats stats(4);
    stats.record_sample(5000, 4000, false); // negative delta clamped to 0
    for (uint64_t i = 1; i <= 10; ++i) {
        stats.record_ns(i * 500, false);
    }
    const auto s = stats.summarize();
    KBNL_EXPECT_FALSE(s.exact_quantiles);
    KBNL_EXPECT_EQ(s.negative_clamped, 1ULL);
    KBNL_EXPECT_EQ(s.count, 11ULL);
    KBNL_EXPECT_EQ(s.min_ns, 0ULL);
    KBNL_EXPECT_EQ(s.max_ns, 5000ULL);
    KBNL_EXPECT_TRUE(s.p50_ns <= s.p99_ns);
    KBNL_EXPECT_TRUE(s.p99_ns <= s.max_ns);
}

KBNL_TEST(deterministic_fixture_matches_independent_oracle) {
    const std::string bin_path = std::string(KBNL_SOURCE_DIR) + "/fixtures/deterministic_feed_v1.bin";
    const auto res = kbnl::run_fixture_replay(bin_path);
    KBNL_EXPECT_TRUE(res.status.ok());
    KBNL_EXPECT_EQ(res.execution_status, std::string("FIXTURE_VERIFICATION"));
    KBNL_EXPECT_TRUE(res.sequence_accounting_verified);
    KBNL_EXPECT_TRUE(res.sequence.accounting_invariant_holds());

    // Exact match against fixtures/expected_reference_result.json
    KBNL_EXPECT_EQ(res.actual_packets_sent, 12ULL);
    KBNL_EXPECT_EQ(res.sequence.total_valid_data_packets, 11ULL);
    KBNL_EXPECT_EQ(res.sequence.unique_received, 10ULL);
    KBNL_EXPECT_EQ(res.sequence.warmup_received, 2ULL);
    KBNL_EXPECT_EQ(res.sequence.measured_received, 8ULL);
    KBNL_EXPECT_EQ(res.sequence.gaps_events, 3ULL);
    KBNL_EXPECT_EQ(res.sequence.missing_within_highest, 2ULL);
    KBNL_EXPECT_EQ(res.inferred_total_lost, 2ULL);
    KBNL_EXPECT_EQ(res.sequence.out_of_order, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.duplicates, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.checksum_errors, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.decode_errors, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.eos_packets, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.first_seq, 1ULL);
    KBNL_EXPECT_EQ(res.sequence.highest_seq, 12ULL);
    KBNL_EXPECT_EQ(res.sequence.last_seq, 12ULL);

    KBNL_EXPECT_EQ(res.latency.count, 8ULL);
    KBNL_EXPECT_EQ(res.latency.warmup_ignored, 2ULL);
    KBNL_EXPECT_TRUE(res.latency.exact_quantiles);
    KBNL_EXPECT_EQ(res.latency.min_ns, 1000ULL);
    KBNL_EXPECT_NEAR(res.latency.mean_ns, 1837.5, 0.01);
    KBNL_EXPECT_EQ(res.latency.p50_ns, 1800ULL);
    KBNL_EXPECT_EQ(res.latency.p95_ns, 3000ULL);
    KBNL_EXPECT_EQ(res.latency.p99_ns, 3000ULL);
    KBNL_EXPECT_EQ(res.latency.p999_ns, 3000ULL);
    KBNL_EXPECT_EQ(res.latency.max_ns, 3000ULL);
}

} // namespace

int main() {
    return kbnl::test::run_all_tests("domain_sequence_and_ring");
}
