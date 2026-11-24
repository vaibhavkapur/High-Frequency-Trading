#include "mdf/fault_injector.hpp"

#include <algorithm>

#include "mdf/protocol.hpp"

namespace mdf {

FaultInjector::FaultInjector(FaultConfig config)
    : config_(std::move(config)), rng_(config_.seed) {}

bool FaultInjector::in_drop_range(uint64_t seq_num) const noexcept {
  for (const auto& r : config_.drop_ranges) {
    if (seq_num >= r.from_seq && seq_num <= r.to_seq) {
      return true;
    }
  }
  return false;
}

uint32_t FaultInjector::sample_jitter_us() {
  if (config_.jitter_max_us == 0) return 0;
  const uint32_t lo = std::min(config_.jitter_min_us, config_.jitter_max_us);
  const uint32_t hi = std::max(config_.jitter_min_us, config_.jitter_max_us);
  if (lo == hi) {
    ++stats_.jitter_applied;
    return lo;
  }
  std::uniform_int_distribution<uint32_t> dist(lo, hi);
  ++stats_.jitter_applied;
  return dist(rng_);
}

std::vector<ScheduledPacket> FaultInjector::process(std::span<const uint8_t> datagram) {
  ++stats_.packets_in;
  ++step_;

  std::vector<ScheduledPacket> emitted;

  // Release any previously delayed packets whose step has arrived.
  auto release_due = [&]() {
    std::vector<DelayedEntry> remaining;
    remaining.reserve(delayed_.size());
    for (auto& d : delayed_) {
      if (d.release_step <= step_) {
        emitted.push_back(std::move(d.pkt));
        ++stats_.packets_out;
      } else {
        remaining.push_back(std::move(d));
      }
    }
    delayed_ = std::move(remaining);
  };

  if (!config_.enabled()) {
    ScheduledPacket pkt{};
    pkt.bytes.assign(datagram.begin(), datagram.end());
    emitted.push_back(std::move(pkt));
    ++stats_.packets_out;
    return emitted;
  }

  // Peek header (without failing if already malformed) to inspect flags & seq_num.
  PacketHeader hdr{};
  const bool valid_hdr = (decode_packet_header(datagram, hdr) == DecodeStatus::kOk);
  const bool is_heartbeat = valid_hdr && ((hdr.flags & kFlagHeartbeat) != 0U);

  if (is_heartbeat && !config_.affect_heartbeats) {
    release_due();
    ScheduledPacket pkt{};
    pkt.bytes.assign(datagram.begin(), datagram.end());
    emitted.push_back(std::move(pkt));
    ++stats_.packets_out;
    return emitted;
  }

  std::uniform_real_distribution<double> u01(0.0, 1.0);

  // 1. Explicit drop range (disconnect/burst loss) or probabilistic drop.
  const bool forced_drop = valid_hdr && in_drop_range(hdr.seq_num);
  if (forced_drop || (config_.drop_prob > 0.0 && u01(rng_) < config_.drop_prob)) {
    ++stats_.dropped;
    release_due();
    return emitted;
  }

  ScheduledPacket pkt{};
  pkt.bytes.assign(datagram.begin(), datagram.end());
  pkt.jitter_us = sample_jitter_us();

  // 2. Bit corruption (mutates a payload/header byte to trigger CRC32 failure).
  if (config_.corrupt_prob > 0.0 && !pkt.bytes.empty() && u01(rng_) < config_.corrupt_prob) {
    std::uniform_int_distribution<std::size_t> idx_dist(0, pkt.bytes.size() - 1);
    const std::size_t idx = idx_dist(rng_);
    pkt.bytes[idx] ^= 0x01U;
    ++stats_.corrupted;
  }

  // 3. Reordering (delay this packet by 1..max_reorder_delay steps).
  if (config_.reorder_prob > 0.0 && config_.max_reorder_delay > 0 &&
      u01(rng_) < config_.reorder_prob) {
    std::uniform_int_distribution<uint32_t> delay_dist(1, config_.max_reorder_delay);
    const uint64_t release_step = step_ + delay_dist(rng_);
    delayed_.push_back(DelayedEntry{release_step, std::move(pkt)});
    ++stats_.reordered;
    release_due();
    return emitted;
  }

  // Emit current packet, optionally duplicated, and release any due delayed packets.
  const bool dup = (config_.duplicate_prob > 0.0 && u01(rng_) < config_.duplicate_prob);
  if (dup) {
    ScheduledPacket copy = pkt;
    emitted.push_back(std::move(pkt));
    ++stats_.packets_out;
    emitted.push_back(std::move(copy));
    ++stats_.packets_out;
    ++stats_.duplicated;
  } else {
    emitted.push_back(std::move(pkt));
    ++stats_.packets_out;
  }

  release_due();
  return emitted;
}

std::vector<ScheduledPacket> FaultInjector::flush() {
  std::vector<ScheduledPacket> out;
  out.reserve(delayed_.size());
  for (auto& d : delayed_) {
    out.push_back(std::move(d.pkt));
    ++stats_.packets_out;
  }
  delayed_.clear();
  return out;
}

void FaultInjector::reset() {
  rng_.seed(config_.seed);
  stats_ = FaultStats{};
  step_ = 0;
  delayed_.clear();
}

}  // namespace mdf
