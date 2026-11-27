#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obce/types.hpp"

namespace obce {

struct EncodedStream {
    StreamHeader header;
    std::vector<uint8_t> bytes;
    std::vector<CheckpointEntry> checkpoints;
    DiagnosticCounters counters;
};

struct DecodedStream {
    StreamHeader header;
    std::vector<MarketEvent> events;
    std::vector<CheckpointEntry> checkpoints;
    DiagnosticCounters counters;
};

// Stateful incremental delta/varint encoder with periodic checkpoints and optional index footer.
// Completely isolated from file/network I/O.
class DeltaEncoder {
public:
    explicit DeltaEncoder(const CodecConfig& config);

    // Validate configuration and initialize stream header
    Status reset();

    // Append a single event; flushes checkpoint or delta frames automatically
    Status push(const MarketEvent& event);

    // Flush any buffered events and optionally emit the seekable checkpoint index footer
    Result<EncodedStream> finish();

    // Convenience one-shot encode
    static Result<EncodedStream> encode_all(
        const std::vector<MarketEvent>& events,
        const CodecConfig& config);

    const DiagnosticCounters& counters() const noexcept { return counters_; }

private:
    Status flush_checkpoint_frame(const MarketEvent& anchor_event);
    Status flush_delta_frame();
    Status append_index_footer();
    void append_frame(FrameType type, uint32_t event_count, uint64_t first_seq, const std::vector<uint8_t>& payload);

    CodecConfig config_;
    StreamHeader header_;
    bool initialized_ = false;
    bool finished_ = false;
    bool has_last_event_ = false;
    uint64_t total_events_ = 0;
    uint32_t events_since_checkpoint_ = 0;
    MarketEvent last_event_{};
    MarketEvent delta_base_event_{};
    std::vector<MarketEvent> pending_deltas_;
    std::vector<uint8_t> buffer_;
    std::vector<CheckpointEntry> checkpoints_;
    DiagnosticCounters counters_;
};

// Stateless/indexed decoder supporting:
//  - Full sequential decode with strict CRC32C verification
//  - Sequence gap detection and recovery from missing/corrupt delta frames (ResyncAtCheckpoint)
//  - Schema evolution (v1 streams transparently decoded into v2 MarketEvent struct with zeroed defaults)
//  - Random access by sequence ID or event index via checkpoint index footer (or linear checkpoint scan)
class DeltaDecoder {
public:
    static Result<DecodedStream> decode_all(
        const uint8_t* data,
        size_t len,
        GapPolicy gap_policy = GapPolicy::StrictReject);

    static Result<DecodedStream> decode_all(
        const std::vector<uint8_t>& bytes,
        GapPolicy gap_policy = GapPolicy::StrictReject) {
        return decode_all(bytes.data(), bytes.size(), gap_policy);
    }

    // Read checkpoint index (O(1) if IndexFooter is present, else scans frame headers)
    static Result<std::vector<CheckpointEntry>> read_checkpoint_index(
        const uint8_t* data,
        size_t len,
        StreamHeader* out_header = nullptr);

    // Random-access lookup for a specific sequence ID using the checkpoint index.
    // Seeks directly to the nearest preceding checkpoint and decodes forward at most checkpoint_interval events.
    static Result<MarketEvent> seek_by_seq_id(
        const uint8_t* data,
        size_t len,
        uint64_t target_seq_id,
        const std::vector<CheckpointEntry>& index,
        const StreamHeader& header,
        DiagnosticCounters* counters = nullptr);

    // Random-access lookup for a contiguous range [start_seq_id, end_seq_id] inclusive.
    static Result<std::vector<MarketEvent>> decode_seq_range(
        const uint8_t* data,
        size_t len,
        uint64_t start_seq_id,
        uint64_t end_seq_id,
        const std::vector<CheckpointEntry>& index,
        const StreamHeader& header,
        DiagnosticCounters* counters = nullptr);
};

}  // namespace obce
