#pragma once

#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace mdf {

struct SeqRange {
  uint64_t from_seq = 0;
  uint64_t to_seq = 0;  // Inclusive
};

struct FaultConfig {
  uint64_t seed = 42;
  double drop_prob = 0.0;        // Independent per-packet drop probability [0, 1]
  double reorder_prob = 0.0;     // Probability to delay a packet by 1..max_reorder_delay packets
  uint32_t max_reorder_delay = 3;
  double duplicate_prob = 0.0;   // Probability to duplicate a packet
  double corrupt_prob = 0.0;     // Probability to flip a bit in a packet (tests CRC32 rejection)
  uint32_t jitter_min_us = 0;    // Optional sleep jitter min (microseconds)
  uint32_t jitter_max_us = 0;    // Optional sleep jitter max (microseconds)

  // Explicit deterministic sequence drop ranges (e.g., disconnect simulation or burst loss).
  std::vector<SeqRange> drop_ranges;

  // Never drop heartbeat packets if false (default: heartbeats pass so tail gaps are visible).
  bool affect_heartbeats = false;

  bool enabled() const noexcept {
    return drop_prob > 0.0 || reorder_prob > 0.0 || duplicate_prob > 0.0 || corrupt_prob > 0.0 ||
           jitter_max_us > 0 || !drop_ranges.empty();
  }
};

struct FaultStats {
  uint64_t packets_in = 0;
  uint64_t packets_out = 0;
  uint64_t dropped = 0;
  uint64_t reordered = 0;
  uint64_t duplicated = 0;
  uint64_t corrupted = 0;
  uint64_t jitter_applied = 0;
};

struct ScheduledPacket {
  std::vector<uint8_t> bytes;
  uint32_t jitter_us = 0;
};

// Deterministic fault injector operating on raw MDF1 datagrams.
// Can be placed either on the publisher egress path or in front of a specific subscriber's
// ingestion path so an uninterrupted subscriber and a faulted subscriber share one live stream.
class FaultInjector {
 public:
  explicit FaultInjector(FaultConfig config = {});

  // Process one incoming datagram and return 0, 1, or more datagrams to emit now.
  std::vector<ScheduledPacket> process(std::span<const uint8_t> datagram);

  // Flush any remaining delayed (reordered) packets at end of stream.
  std::vector<ScheduledPacket> flush();

  void reset();
  const FaultConfig& config() const noexcept { return config_; }
  const FaultStats& stats() const noexcept { return stats_; }

 private:
  struct DelayedEntry {
    uint64_t release_step = 0;
    ScheduledPacket pkt;
  };

  bool in_drop_range(uint64_t seq_num) const noexcept;
  uint32_t sample_jitter_us();

  FaultConfig config_;
  FaultStats stats_{};
  std::mt19937_64 rng_;
  uint64_t step_ = 0;
  std::vector<DelayedEntry> delayed_;
};

}  // namespace mdf
