#include "mdf/channel_receiver.hpp"

#include <algorithm>

namespace mdf {

const char* receiver_state_name(ReceiverState state) noexcept {
  switch (state) {
    case ReceiverState::kLive:
      return "LIVE";
    case ReceiverState::kGapWait:
      return "GAP_WAIT";
    case ReceiverState::kSyncingSnapshot:
      return "SYNCING_SNAPSHOT";
  }
  return "UNKNOWN";
}

ChannelReceiver::ChannelReceiver(ChannelReceiverConfig cfg, IRecoveryClient* recovery_client)
    : cfg_(cfg), recovery_client_(recovery_client), market_state_(cfg.channel_id) {}

void ChannelReceiver::enter_gap_wait(uint64_t seen_seq, uint64_t now_ns) {
  if (seen_seq > highest_seen_seq_) {
    highest_seen_seq_ = seen_seq;
  }
  if (state_ == ReceiverState::kLive) {
    state_ = ReceiverState::kGapWait;
    gap_detected_ts_ns_ = now_ns;
    gap_detected_wall_ns_ = steady_now_ns();
    packets_since_gap_ = 0;
    ++stats_.gaps_detected;
    const uint64_t missing = (seen_seq >= expected_seq_) ? (seen_seq - expected_seq_ + 1) : 1;
    stats_.missing_packets_detected += missing;
    if (on_gap_alarm_) {
      on_gap_alarm_(GapAlarm{cfg_.channel_id, expected_seq_, seen_seq, missing});
    }
  }
}

void ChannelReceiver::buffer_future_packet(DecodedPacket pkt) {
  const uint64_t seq = pkt.header.seq_num;
  if (reorder_buffer_.find(seq) != reorder_buffer_.end()) {
    ++stats_.duplicate_packets;
    return;
  }
  if (reorder_buffer_.size() >= cfg_.max_reorder_buffer) {
    // Bounded memory: drop the incoming furthest-future packet rather than growing unboundedly.
    ++stats_.buffer_overflow_drops;
    return;
  }
  reorder_buffer_.emplace(seq, std::move(pkt));
}

void ChannelReceiver::drain_reorder_buffer() {
  while (!reorder_buffer_.empty()) {
    auto it = reorder_buffer_.begin();
    if (it->first < expected_seq_) {
      ++stats_.stale_incrementals_discarded;
      reorder_buffer_.erase(it);
      continue;
    }
    if (it->first != expected_seq_) {
      break;
    }
    market_state_.apply_packet(it->first, it->second.events);
    ++stats_.packets_applied;
    stats_.events_applied += it->second.events.size();
    ++stats_.reordered_recovered_packets;
    ++expected_seq_;
    reorder_buffer_.erase(it);
  }
}

void ChannelReceiver::finish_gap_if_synced(uint64_t now_ns) {
  (void)now_ns;
  if (state_ != ReceiverState::kLive && reorder_buffer_.empty() &&
      expected_seq_ > highest_seen_seq_) {
    const uint64_t done_wall_ns = steady_now_ns();
    if (gap_detected_wall_ns_ > 0 && done_wall_ns >= gap_detected_wall_ns_) {
      recovery_times_ns_.push_back(done_wall_ns - gap_detected_wall_ns_);
    }
    state_ = ReceiverState::kLive;
    gap_detected_ts_ns_ = 0;
    gap_detected_wall_ns_ = 0;
    packets_since_gap_ = 0;
  }
}

bool ChannelReceiver::should_trigger_recovery(uint64_t now_ns) const noexcept {
  if (state_ != ReceiverState::kGapWait) return false;
  if (packets_since_gap_ >= cfg_.reorder_window_packets) return true;
  if (highest_seen_seq_ >= expected_seq_ + cfg_.reorder_window_packets) return true;
  if (reorder_buffer_.size() >= cfg_.max_reorder_buffer) return true;
  if (cfg_.reorder_timeout_us > 0 && gap_detected_ts_ns_ > 0 && now_ns >= gap_detected_ts_ns_) {
    const uint64_t elapsed_us = (now_ns - gap_detected_ts_ns_) / 1000ULL;
    if (elapsed_us >= cfg_.reorder_timeout_us) return true;
  }
  return false;
}

DecodeStatus ChannelReceiver::on_datagram(std::span<const uint8_t> datagram, uint64_t recv_ts_ns) {
  ++stats_.datagrams_processed;

  DecodedPacket pkt{};
  const DecodeStatus status = decode_packet(datagram, pkt);
  if (status != DecodeStatus::kOk) {
    if (status == DecodeStatus::kBadChecksum) {
      ++stats_.checksum_errors;
    } else {
      ++stats_.malformed_packets;
    }
    return status;
  }

  if (pkt.header.channel_id != cfg_.channel_id) {
    ++stats_.wrong_channel_packets;
    return DecodeStatus::kOk;
  }

  // Heartbeat: does not advance sequence number, but reveals silent tail drops.
  if ((pkt.header.flags & kFlagHeartbeat) != 0U) {
    ++stats_.heartbeats_received;
    if (pkt.header.seq_num >= expected_seq_) {
      enter_gap_wait(pkt.header.seq_num, recv_ts_ns);
      // Heartbeat confirms publisher already moved past expected_seq_; recover promptly.
      if (recovery_client_ != nullptr) {
        (void)recover_gaps(recv_ts_ns);
      }
    }
    return DecodeStatus::kOk;
  }

  const uint64_t seq = pkt.header.seq_num;
  if (seq == 0) {
    ++stats_.malformed_packets;
    return DecodeStatus::kInvalidField;
  }

  if (cfg_.collect_latency_samples && pkt.header.send_ts_ns > 0 &&
      recv_ts_ns >= pkt.header.send_ts_ns) {
    e2e_latencies_ns_.push_back(recv_ts_ns - pkt.header.send_ts_ns);
  }

  if (seq < expected_seq_) {
    ++stats_.duplicate_packets;
    return DecodeStatus::kOk;
  }

  if (seq == expected_seq_) {
    if (seq > highest_seen_seq_) {
      highest_seen_seq_ = seq;
    }
    market_state_.apply_packet(seq, pkt.events);
    ++stats_.packets_applied;
    stats_.events_applied += pkt.events.size();
    ++expected_seq_;

    drain_reorder_buffer();
    finish_gap_if_synced(recv_ts_ns);

    if (state_ == ReceiverState::kGapWait && should_trigger_recovery(recv_ts_ns)) {
      (void)recover_gaps(recv_ts_ns);
    }
    return DecodeStatus::kOk;
  }

  // seq > expected_seq_: out-of-order or packet drop!
  ++stats_.out_of_order_packets;
  const bool was_live = (state_ == ReceiverState::kLive);
  if (was_live) {
    // Missing count is [expected_seq_, seq - 1].
    state_ = ReceiverState::kGapWait;
    gap_detected_ts_ns_ = recv_ts_ns;
    gap_detected_wall_ns_ = steady_now_ns();
    packets_since_gap_ = 0;
    ++stats_.gaps_detected;
    const uint64_t missing = seq - expected_seq_;
    stats_.missing_packets_detected += missing;
    if (on_gap_alarm_) {
      on_gap_alarm_(GapAlarm{cfg_.channel_id, expected_seq_, seq, missing});
    }
  }
  if (seq > highest_seen_seq_) {
    highest_seen_seq_ = seq;
  }
  ++packets_since_gap_;
  buffer_future_packet(std::move(pkt));

  if (should_trigger_recovery(recv_ts_ns)) {
    (void)recover_gaps(recv_ts_ns);
  }
  return DecodeStatus::kOk;
}

void ChannelReceiver::tick(uint64_t now_ns) {
  if (should_trigger_recovery(now_ns)) {
    (void)recover_gaps(now_ns);
  }
}

bool ChannelReceiver::flush_recovery(uint64_t now_ns) {
  if (state_ == ReceiverState::kLive && reorder_buffer_.empty()) {
    return true;
  }
  return recover_gaps(now_ns);
}

void ChannelReceiver::apply_snapshot(const SnapshotData& snap) {
  market_state_.load_snapshot(snap);
  expected_seq_ = snap.as_of_seq + 1;
  if (snap.as_of_seq > highest_seen_seq_) {
    highest_seen_seq_ = snap.as_of_seq;
  }
  ++stats_.snapshots_applied;

  // Critical invariant: discard any staged/buffered incrementals with seq <= snap.as_of_seq
  // so stale incrementals are NEVER merged into fresh snapshot state.
  while (!reorder_buffer_.empty() && reorder_buffer_.begin()->first <= snap.as_of_seq) {
    reorder_buffer_.erase(reorder_buffer_.begin());
    ++stats_.stale_incrementals_discarded;
  }

  // Replay contiguous staged incrementals strictly beyond snap.as_of_seq.
  drain_reorder_buffer();
}

bool ChannelReceiver::apply_retransmitted_range(uint64_t from_seq,
                                                uint64_t to_seq,
                                                bool& escalated_unavailable) {
  escalated_unavailable = false;
  if (recovery_client_ == nullptr) return false;

  uint64_t cur = from_seq;
  const uint64_t chunk_max = std::max<uint64_t>(1, cfg_.max_retransmit_chunk);

  while (cur <= to_seq) {
    const uint64_t chunk_end = std::min(to_seq, cur + chunk_max - 1);
    RetransmitRequest req{cfg_.channel_id, cur, chunk_end};
    RetransmitResponse resp{};
    ++stats_.retransmit_requests;
    if (!recovery_client_->request_retransmit(req, resp)) {
      ++stats_.recovery_failures;
      return false;
    }

    if (resp.status == RecoveryStatus::kUnavailable) {
      ++stats_.retransmit_unavailable_responses;
      escalated_unavailable = true;
      return false;
    }
    if (resp.status != RecoveryStatus::kOk) {
      ++stats_.recovery_failures;
      return false;
    }

    const uint64_t expected_count = chunk_end - cur + 1;
    if (resp.raw_packets.size() != expected_count) {
      ++stats_.recovery_failures;
      return false;
    }

    for (const auto& raw : resp.raw_packets) {
      DecodedPacket pkt{};
      if (decode_packet(raw, pkt) != DecodeStatus::kOk ||
          pkt.header.channel_id != cfg_.channel_id || pkt.header.seq_num != expected_seq_) {
        ++stats_.recovery_failures;
        return false;
      }
      market_state_.apply_packet(pkt.header.seq_num, pkt.events);
      ++stats_.packets_applied;
      stats_.events_applied += pkt.events.size();
      ++stats_.retransmit_packets_recovered;
      ++expected_seq_;
      if (pkt.header.seq_num > highest_seen_seq_) {
        highest_seen_seq_ = pkt.header.seq_num;
      }
    }
    cur = chunk_end + 1;
  }

  return true;
}

bool ChannelReceiver::recover_via_snapshot(uint64_t now_ns) {
  if (recovery_client_ == nullptr) return false;
  state_ = ReceiverState::kSyncingSnapshot;
  ++stats_.snapshot_requests;

  SnapshotRequest req{cfg_.channel_id};
  SnapshotResponse resp{};
  if (!recovery_client_->request_snapshot(req, resp) || resp.status != RecoveryStatus::kOk) {
    ++stats_.recovery_failures;
    state_ = ReceiverState::kGapWait;
    return false;
  }

  apply_snapshot(resp.snapshot);

  // If the feed has advanced beyond the snapshot checkpoint, close any remaining hole(s)
  // between expected_seq_ (= snap.as_of_seq + 1) and highest_seen_seq_.
  for (int guard = 0; guard < 16; ++guard) {
    drain_reorder_buffer();
    if (reorder_buffer_.empty() && expected_seq_ > highest_seen_seq_) {
      finish_gap_if_synced(now_ns);
      return true;
    }
    const uint64_t hole_end =
        !reorder_buffer_.empty() ? (reorder_buffer_.begin()->first - 1) : highest_seen_seq_;
    if (hole_end < expected_seq_) {
      drain_reorder_buffer();
      continue;
    }
    bool unavailable = false;
    if (!apply_retransmitted_range(expected_seq_, hole_end, unavailable)) {
      state_ = ReceiverState::kGapWait;
      return false;
    }
  }

  drain_reorder_buffer();
  finish_gap_if_synced(now_ns);
  return state_ == ReceiverState::kLive;
}

bool ChannelReceiver::recover_gaps(uint64_t now_ns) {
  if (recovery_client_ == nullptr) return false;

  for (int guard = 0; guard < 32; ++guard) {
    drain_reorder_buffer();
    if (reorder_buffer_.empty() && expected_seq_ > highest_seen_seq_) {
      finish_gap_if_synced(now_ns);
      return true;
    }

    const uint64_t hole_end =
        !reorder_buffer_.empty() ? (reorder_buffer_.begin()->first - 1) : highest_seen_seq_;
    if (hole_end < expected_seq_) {
      drain_reorder_buffer();
      continue;
    }

    // If the gap itself already exceeds max_retransmit_chunk, check retransmit once (or let server
    // tell us UNAVAILABLE) so policy decision is visible in stats, then escalate to snapshot.
    bool unavailable = false;
    if (apply_retransmitted_range(expected_seq_, hole_end, unavailable)) {
      drain_reorder_buffer();
      continue;
    }
    if (unavailable) {
      return recover_via_snapshot(now_ns);
    }
    return false;
  }

  drain_reorder_buffer();
  finish_gap_if_synced(now_ns);
  return state_ == ReceiverState::kLive;
}

}  // namespace mdf
