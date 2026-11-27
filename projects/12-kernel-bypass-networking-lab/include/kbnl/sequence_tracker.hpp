#pragma once

#include "kbnl/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kbnl {

enum class PacketObservation {
    kInOrder = 0,
    kGapDetected,
    kOutOfOrder,
    kDuplicate,
    kTooOldOutOfWindow,
    kEndOfStream
};

struct SequenceCounters {
    uint64_t total_valid_data_packets = 0;
    uint64_t unique_received = 0;
    uint64_t warmup_received = 0;
    uint64_t measured_received = 0;
    uint64_t burst_packets = 0;
    uint64_t gaps_events = 0;
    uint64_t missing_within_highest = 0;
    uint64_t out_of_order = 0;
    uint64_t duplicates = 0;
    uint64_t too_old_out_of_window = 0;
    uint64_t checksum_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t ring_drops = 0;
    uint64_t eos_packets = 0;

    uint64_t first_seq = 0;
    uint64_t highest_seq = 0;
    uint64_t last_seq = 0;

    // Verifies the core accounting invariant for 1-based sequence streams:
    //   unique_received + missing_within_highest == highest_seq (when first_seq == 1 or starting from 1)
    //   total_valid_data_packets == unique_received + duplicates + too_old_out_of_window
    bool accounting_invariant_holds() const noexcept;

    // Computes total lost packets relative to a known expected total sent count.
    uint64_t lost_against_expected(uint64_t expected_total_sent) const noexcept;
};

// Pure, deterministic sequence tracker with a bounded 1024-sequence sliding reorder window.
// Isolated from all I/O and wall clocks.
class SequenceTracker {
public:
    static constexpr size_t kReorderWindowSize = 1024;

    SequenceTracker() noexcept;

    void reset() noexcept;

    PacketObservation on_packet(const PacketHeader& header) noexcept;
    void on_decode_error(DecodeError err) noexcept;
    void on_ring_drop(uint64_t count = 1) noexcept;

    const SequenceCounters& counters() const noexcept { return counters_; }

private:
    static constexpr size_t kWordCount = kReorderWindowSize / 64;

    bool test_window_bit(uint64_t seq) const noexcept;
    void set_window_bit(uint64_t seq) noexcept;
    void clear_window_bit(uint64_t seq) noexcept;

    SequenceCounters counters_{};
    std::array<uint64_t, kWordCount> seen_bitmap_{};
};

} // namespace kbnl
