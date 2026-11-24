#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

#include "mdf/order_book.hpp"
#include "mdf/protocol.hpp"
#include "mdf/recovery_service.hpp"

namespace mdf {

enum class ReceiverState : uint8_t {
  kLive = 0,
  kGapWait = 1,
  kSyncingSnapshot = 2,
};

const char* receiver_state_name(ReceiverState state) noexcept;

struct GapAlarm {
  uint16_t channel_id = 0;
  uint64_t expected_seq = 0;
  uint64_t received_seq = 0;
  uint64_t missing_count = 0;
};

struct ChannelReceiverConfig {
  uint16_t channel_id = 0;
  // Number of ahead packets (or sequence distance) to tolerate before invoking TCP recovery,
  // giving naturally reordered UDP datagrams a chance to arrive first.
  uint32_t reorder_window_packets = 4;
  // Maximum time (microseconds) to wait in kGapWait before triggering recovery.
  uint64_t reorder_timeout_us = 2000;
  // Upper bound on buffered out-of-order / snapshot-staged packets.
  std::size_t max_reorder_buffer = 1024;
  // Maximum sequence range requested per single TCP RETRANSMIT_REQ chunk.
  uint64_t max_retransmit_chunk = 256;
  // Record per-packet e2e latency samples (disable if running huge memory-constrained streams).
  bool collect_latency_samples = true;
};

struct ChannelStats {
  uint64_t datagrams_processed = 0;
  uint64_t packets_applied = 0;
  uint64_t events_applied = 0;
  uint64_t heartbeats_received = 0;
  uint64_t wrong_channel_packets = 0;
  uint64_t checksum_errors = 0;
  uint64_t malformed_packets = 0;
  uint64_t duplicate_packets = 0;
  uint64_t out_of_order_packets = 0;
  uint64_t reordered_recovered_packets = 0;
  uint64_t buffer_overflow_drops = 0;
  uint64_t gaps_detected = 0;
  uint64_t missing_packets_detected = 0;
  uint64_t retransmit_requests = 0;
  uint64_t retransmit_packets_recovered = 0;
  uint64_t retransmit_unavailable_responses = 0;
  uint64_t snapshot_requests = 0;
  uint64_t snapshots_applied = 0;
  uint64_t stale_incrementals_discarded = 0;
  uint64_t recovery_failures = 0;
};

// Per-channel UDP market-data receiver with:
// - CRC32 / framing validation
// - Sequence-gap detection & gap alarms
// - Bounded reordering window
// - Duplicate / stale rejection
// - Automatic TCP retransmission with fallback to Snapshot + Incremental synchronization
class ChannelReceiver {
 public:
  ChannelReceiver(ChannelReceiverConfig cfg, IRecoveryClient* recovery_client = nullptr);

  // Ingest one raw UDP datagram received at `recv_ts_ns`.
  // Returns DecodeStatus (kOk even if duplicate/buffered; error status on corrupt/malformed).
  DecodeStatus on_datagram(std::span<const uint8_t> datagram, uint64_t recv_ts_ns);

  // Periodic timer tick to trigger recovery if a gap has persisted longer than reorder_timeout_us.
  void tick(uint64_t now_ns);

  // Force immediate recovery attempt if currently in kGapWait (e.g., at end of stream or flush).
  bool flush_recovery(uint64_t now_ns);

  // Directly apply a snapshot at `snap.as_of_seq`, discarding any buffered incrementals <= N
  // and replaying contiguous buffered incrementals > N.
  void apply_snapshot(const SnapshotData& snap);

  void set_gap_alarm_callback(std::function<void(const GapAlarm&)> cb) {
    on_gap_alarm_ = std::move(cb);
  }

  ReceiverState state() const noexcept { return state_; }
  uint64_t expected_seq() const noexcept { return expected_seq_; }
  uint64_t highest_seen_seq() const noexcept { return highest_seen_seq_; }
  std::size_t buffered_packets() const noexcept { return reorder_buffer_.size(); }

  const ChannelMarketState& market_state() const noexcept { return market_state_; }
  const ChannelStats& stats() const noexcept { return stats_; }
  const std::vector<uint64_t>& e2e_latencies_ns() const noexcept { return e2e_latencies_ns_; }
  const std::vector<uint64_t>& recovery_times_ns() const noexcept { return recovery_times_ns_; }

 private:
  void enter_gap_wait(uint64_t seen_seq, uint64_t now_ns);
  void drain_reorder_buffer();
  void finish_gap_if_synced(uint64_t now_ns);
  bool should_trigger_recovery(uint64_t now_ns) const noexcept;
  bool recover_gaps(uint64_t now_ns);
  bool recover_via_snapshot(uint64_t now_ns);
  bool apply_retransmitted_range(uint64_t from_seq, uint64_t to_seq, bool& escalated_unavailable);
  void buffer_future_packet(DecodedPacket pkt);

  ChannelReceiverConfig cfg_{};
  IRecoveryClient* recovery_client_ = nullptr;
  ChannelMarketState market_state_;
  ChannelStats stats_{};

  ReceiverState state_ = ReceiverState::kLive;
  uint64_t expected_seq_ = 1;
  uint64_t highest_seen_seq_ = 0;
  uint64_t gap_detected_ts_ns_ = 0;
  uint64_t gap_detected_wall_ns_ = 0;
  uint64_t packets_since_gap_ = 0;

  std::map<uint64_t, DecodedPacket> reorder_buffer_;
  std::vector<uint64_t> e2e_latencies_ns_;
  std::vector<uint64_t> recovery_times_ns_;
  std::function<void(const GapAlarm&)> on_gap_alarm_;
};

}  // namespace mdf
