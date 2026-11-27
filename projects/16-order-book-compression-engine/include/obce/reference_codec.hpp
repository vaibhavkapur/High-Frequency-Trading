#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obce/types.hpp"

namespace obce {

// Canonical fixed-width little-endian binary record sizes (excluding frame/stream headers):
// Schema v1 record: 45 bytes
//   seq_id(8) + timestamp_ns(8) + symbol_id(4) + order_id(8) + price_ticks(8) + quantity(8)
//   + packed_meta(1: type[3:0] | side[5:4] | scope[7:6]) + level_index(2) = 47 bytes
// Schema v2 record: 59 bytes
//   v1(47) + venue_id(2) + flags(2) + match_id(8) = 59 bytes
inline constexpr size_t kRefRecordSizeV1 = 47;
inline constexpr size_t kRefRecordSizeV2 = 59;
inline constexpr size_t kStreamHeaderSize = 24;
inline constexpr size_t kFrameHeaderSize = 20;
inline constexpr size_t kFrameCrcSize = 4;

// Encode/decode the 24-byte stream header
Status encode_stream_header(const StreamHeader& hdr, std::vector<uint8_t>& out);
Result<StreamHeader> decode_stream_header(const uint8_t* data, size_t len, size_t& offset);

// Fixed-width record serialization (no heap allocation on hot path)
size_t reference_record_size(uint16_t schema_version) noexcept;
Status encode_reference_event(const MarketEvent& ev, uint16_t schema_version, uint8_t* dst, size_t dst_len) noexcept;
Status decode_reference_event(const uint8_t* src, size_t src_len, uint16_t schema_version, MarketEvent& out) noexcept;

// Reference stream encoder/decoder (framed with CRC32C)
class ReferenceCodec {
public:
    static Result<std::vector<uint8_t>> encode_stream(
        const std::vector<MarketEvent>& events,
        const CodecConfig& config,
        DiagnosticCounters* counters = nullptr);

    static Result<std::vector<MarketEvent>> decode_stream(
        const uint8_t* data,
        size_t len,
        GapPolicy gap_policy = GapPolicy::StrictReject,
        DiagnosticCounters* counters = nullptr,
        StreamHeader* out_header = nullptr);

    static Result<std::vector<MarketEvent>> decode_stream(
        const std::vector<uint8_t>& bytes,
        GapPolicy gap_policy = GapPolicy::StrictReject,
        DiagnosticCounters* counters = nullptr,
        StreamHeader* out_header = nullptr) {
        return decode_stream(bytes.data(), bytes.size(), gap_policy, counters, out_header);
    }
};

}  // namespace obce
