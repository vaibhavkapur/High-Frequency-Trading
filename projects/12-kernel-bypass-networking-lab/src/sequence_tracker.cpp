#include "kbnl/sequence_tracker.hpp"

namespace kbnl {

bool SequenceCounters::accounting_invariant_holds() const noexcept {
    if (total_valid_data_packets != unique_received + duplicates + too_old_out_of_window) {
        return false;
    }
    if (highest_seq == 0) {
        return unique_received == 0 && missing_within_highest == 0;
    }
    return (unique_received + missing_within_highest) == highest_seq;
}

uint64_t SequenceCounters::lost_against_expected(uint64_t expected_total_sent) const noexcept {
    if (expected_total_sent <= unique_received) {
        return 0;
    }
    return expected_total_sent - unique_received;
}

SequenceTracker::SequenceTracker() noexcept {
    reset();
}

void SequenceTracker::reset() noexcept {
    counters_ = SequenceCounters{};
    seen_bitmap_.fill(0);
}

bool SequenceTracker::test_window_bit(uint64_t seq) const noexcept {
    const size_t idx = static_cast<size_t>(seq % kReorderWindowSize);
    return (seen_bitmap_[idx / 64] & (1ULL << (idx % 64))) != 0;
}

void SequenceTracker::set_window_bit(uint64_t seq) noexcept {
    const size_t idx = static_cast<size_t>(seq % kReorderWindowSize);
    seen_bitmap_[idx / 64] |= (1ULL << (idx % 64));
}

void SequenceTracker::clear_window_bit(uint64_t seq) noexcept {
    const size_t idx = static_cast<size_t>(seq % kReorderWindowSize);
    seen_bitmap_[idx / 64] &= ~(1ULL << (idx % 64));
}

PacketObservation SequenceTracker::on_packet(const PacketHeader& header) noexcept {
    if ((header.flags & kFlagEndOfStream) != 0) {
        ++counters_.eos_packets;
        return PacketObservation::kEndOfStream;
    }

    const uint64_t seq = header.seq_num;
    ++counters_.total_valid_data_packets;
    counters_.last_seq = seq;

    if (counters_.first_seq == 0) {
        counters_.first_seq = seq;
    }
    if ((header.flags & kFlagBurst) != 0) {
        ++counters_.burst_packets;
    }

    auto record_unique_category = [&]() {
        ++counters_.unique_received;
        if ((header.flags & kFlagWarmup) != 0) {
            ++counters_.warmup_received;
        } else {
            ++counters_.measured_received;
        }
    };

    if (counters_.highest_seq == 0) {
        seen_bitmap_.fill(0);
        set_window_bit(seq);
        counters_.highest_seq = seq;
        record_unique_category();
        if (seq > 1) {
            ++counters_.gaps_events;
            counters_.missing_within_highest += (seq - 1);
            return PacketObservation::kGapDetected;
        }
        return PacketObservation::kInOrder;
    }

    if (seq > counters_.highest_seq) {
        const uint64_t delta = seq - counters_.highest_seq;
        if (delta >= kReorderWindowSize) {
            seen_bitmap_.fill(0);
        } else {
            for (uint64_t s = counters_.highest_seq + 1; s < seq; ++s) {
                clear_window_bit(s);
            }
        }
        set_window_bit(seq);
        counters_.highest_seq = seq;
        record_unique_category();

        if (delta > 1) {
            ++counters_.gaps_events;
            counters_.missing_within_highest += (delta - 1);
            return PacketObservation::kGapDetected;
        }
        return PacketObservation::kInOrder;
    }

    // seq <= highest_seq
    const uint64_t age = counters_.highest_seq - seq;
    if (age >= kReorderWindowSize) {
        ++counters_.too_old_out_of_window;
        return PacketObservation::kTooOldOutOfWindow;
    }

    if (test_window_bit(seq)) {
        ++counters_.duplicates;
        return PacketObservation::kDuplicate;
    }

    // Delayed out-of-order packet filling a previously observed gap.
    set_window_bit(seq);
    record_unique_category();
    ++counters_.out_of_order;
    if (counters_.missing_within_highest > 0) {
        --counters_.missing_within_highest;
    }
    return PacketObservation::kOutOfOrder;
}

void SequenceTracker::on_decode_error(DecodeError err) noexcept {
    if (err == DecodeError::kOk) {
        return;
    }
    ++counters_.decode_errors;
    if (err == DecodeError::kBadChecksum) {
        ++counters_.checksum_errors;
    }
}

void SequenceTracker::on_ring_drop(uint64_t count) noexcept {
    counters_.ring_drops += count;
}

} // namespace kbnl
