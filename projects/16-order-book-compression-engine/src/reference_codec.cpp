#include "obce/reference_codec.hpp"

#include <cstring>

#include "obce/crc32.hpp"

namespace obce {

namespace {

bool is_valid_event_type(uint8_t raw) noexcept {
    return raw >= static_cast<uint8_t>(EventType::AddOrder) &&
           raw <= static_cast<uint8_t>(EventType::BookClear);
}

bool is_valid_side(uint8_t raw) noexcept {
    return raw <= static_cast<uint8_t>(Side::Ask);
}

bool is_valid_scope(uint8_t raw) noexcept {
    return raw == static_cast<uint8_t>(BookScope::L2) ||
           raw == static_cast<uint8_t>(BookScope::L3);
}

uint8_t pack_meta(EventType type, Side side, BookScope scope) noexcept {
    const uint8_t t = static_cast<uint8_t>(type) & 0x0FU;
    const uint8_t s = static_cast<uint8_t>(side) & 0x03U;
    const uint8_t sc = (scope == BookScope::L3) ? 1U : 0U;
    return static_cast<uint8_t>(t | (s << 4U) | (sc << 6U));
}

bool unpack_meta(uint8_t packed, EventType& type, Side& side, BookScope& scope) noexcept {
    const uint8_t t = packed & 0x0FU;
    const uint8_t s = (packed >> 4U) & 0x03U;
    const uint8_t sc = (packed >> 6U) & 0x01U;
    const uint8_t reserved = (packed >> 7U) & 0x01U;
    if (reserved != 0U || !is_valid_event_type(t) || !is_valid_side(s)) {
        return false;
    }
    type = static_cast<EventType>(t);
    side = static_cast<Side>(s);
    scope = (sc == 1U) ? BookScope::L3 : BookScope::L2;
    return true;
}

}  // namespace

Status validate_event(const MarketEvent& ev, uint16_t schema_version) noexcept {
    if (schema_version != kSchemaVersionV1 && schema_version != kSchemaVersionV2) {
        return Status::Error(ErrorCode::UnsupportedSchemaVersion,
                             "Unsupported schema version: " + std::to_string(schema_version));
    }
    if (ev.seq_id == 0) {
        return Status::Error(ErrorCode::InvalidEvent, "seq_id must be >= 1");
    }
    if (ev.symbol_id == 0) {
        return Status::Error(ErrorCode::InvalidEvent, "symbol_id must be >= 1");
    }
    if (ev.quantity < 0) {
        return Status::Error(ErrorCode::InvalidEvent, "quantity must be non-negative");
    }
    if (!is_valid_event_type(static_cast<uint8_t>(ev.type))) {
        return Status::Error(ErrorCode::InvalidEvent, "invalid event type");
    }
    if (!is_valid_side(static_cast<uint8_t>(ev.side))) {
        return Status::Error(ErrorCode::InvalidEvent, "invalid side");
    }
    if (!is_valid_scope(static_cast<uint8_t>(ev.scope))) {
        return Status::Error(ErrorCode::InvalidEvent, "invalid book scope");
    }
    // L3 order events (Add/Modify/Cancel/Execute) require a non-zero order_id and bid/ask side
    if (ev.scope == BookScope::L3 && ev.type != EventType::BookClear) {
        if (ev.order_id == 0) {
            return Status::Error(ErrorCode::InvalidEvent, "L3 order event requires non-zero order_id");
        }
        if (ev.side == Side::Unknown) {
            return Status::Error(ErrorCode::InvalidEvent, "L3 order event requires Bid or Ask side");
        }
    }
    if (ev.scope == BookScope::L2 &&
        (ev.type == EventType::AddOrder || ev.type == EventType::ModifyOrder ||
         ev.type == EventType::CancelOrder || ev.type == EventType::ExecuteOrder)) {
        return Status::Error(ErrorCode::InvalidEvent, "L2 scope incompatible with L3 order event type");
    }
    if (ev.scope == BookScope::L3 &&
        (ev.type == EventType::L2Update || ev.type == EventType::L2Snapshot)) {
        return Status::Error(ErrorCode::InvalidEvent, "L3 scope incompatible with L2 event type");
    }
    return Status::Ok();
}

Status validate_config(const CodecConfig& cfg) noexcept {
    if (cfg.schema_version != kSchemaVersionV1 && cfg.schema_version != kSchemaVersionV2) {
        return Status::Error(ErrorCode::UnsupportedSchemaVersion,
                             "Unsupported schema version: " + std::to_string(cfg.schema_version));
    }
    if (cfg.price_decimals > 18) {
        return Status::Error(ErrorCode::InvalidArgument, "price_decimals must be in [0, 18]");
    }
    if (cfg.checkpoint_interval == 0 || cfg.checkpoint_interval > kMaxCheckpointInterval) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "checkpoint_interval must be in [1, " + std::to_string(kMaxCheckpointInterval) + "]");
    }
    if (cfg.delta_frame_events == 0 || cfg.delta_frame_events > kMaxEventsPerFrame) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "delta_frame_events must be in [1, " + std::to_string(kMaxEventsPerFrame) + "]");
    }
    return Status::Ok();
}

size_t reference_record_size(uint16_t schema_version) noexcept {
    if (schema_version == kSchemaVersionV1) {
        return kRefRecordSizeV1;
    }
    if (schema_version == kSchemaVersionV2) {
        return kRefRecordSizeV2;
    }
    return 0;
}

uint32_t compute_events_digest(const std::vector<MarketEvent>& events, uint16_t schema_version) noexcept {
    const size_t rec_sz = reference_record_size(schema_version);
    if (rec_sz == 0) {
        return 0;
    }
    uint32_t crc = 0;
    uint8_t buf[kRefRecordSizeV2];
    for (const auto& ev : events) {
        if (encode_reference_event(ev, schema_version, buf, rec_sz).ok()) {
            crc = crc32c_update(crc, buf, rec_sz);
        }
    }
    return crc;
}

Status encode_stream_header(const StreamHeader& hdr, std::vector<uint8_t>& out) {
    if (hdr.schema_version != kSchemaVersionV1 && hdr.schema_version != kSchemaVersionV2) {
        return Status::Error(ErrorCode::UnsupportedSchemaVersion, "Unsupported schema version");
    }
    if (hdr.price_decimals > 18) {
        return Status::Error(ErrorCode::InvalidArgument, "price_decimals out of range");
    }
    if (hdr.checkpoint_interval == 0 || hdr.checkpoint_interval > kMaxCheckpointInterval) {
        return Status::Error(ErrorCode::InvalidArgument, "checkpoint_interval out of range");
    }
    const size_t start = out.size();
    out.resize(start + kStreamHeaderSize);
    uint8_t* p = out.data() + start;
    write_u32_le(p + 0, kStreamMagic);
    write_u16_le(p + 4, hdr.schema_version);
    p[6] = static_cast<uint8_t>(hdr.codec);
    p[7] = hdr.price_decimals;
    write_u32_le(p + 8, hdr.checkpoint_interval);
    write_u32_le(p + 12, hdr.flags);
    write_u64_le(p + 16, hdr.stream_id);
    return Status::Ok();
}

Result<StreamHeader> decode_stream_header(const uint8_t* data, size_t len, size_t& offset) {
    if (data == nullptr || offset + kStreamHeaderSize > len) {
        return Result<StreamHeader>::Err(ErrorCode::TruncatedInput, "Stream header truncated");
    }
    const uint8_t* p = data + offset;
    StreamHeader hdr;
    hdr.magic = read_u32_le(p + 0);
    if (hdr.magic != kStreamMagic) {
        return Result<StreamHeader>::Err(ErrorCode::InvalidMagic, "Invalid OBCE stream magic");
    }
    hdr.schema_version = read_u16_le(p + 4);
    if (hdr.schema_version != kSchemaVersionV1 && hdr.schema_version != kSchemaVersionV2) {
        return Result<StreamHeader>::Err(ErrorCode::UnsupportedSchemaVersion,
                                         "Unsupported schema version: " + std::to_string(hdr.schema_version));
    }
    const uint8_t raw_codec = p[6];
    if (raw_codec < static_cast<uint8_t>(CodecKind::ReferenceBinary) ||
        raw_codec > static_cast<uint8_t>(CodecKind::DeltaVarintZstd)) {
        return Result<StreamHeader>::Err(ErrorCode::UnsupportedCodec,
                                         "Unsupported codec id: " + std::to_string(raw_codec));
    }
    hdr.codec = static_cast<CodecKind>(raw_codec);
    hdr.price_decimals = p[7];
    if (hdr.price_decimals > 18) {
        return Result<StreamHeader>::Err(ErrorCode::CorruptFrame, "Header price_decimals > 18");
    }
    hdr.checkpoint_interval = read_u32_le(p + 8);
    if (hdr.checkpoint_interval == 0 || hdr.checkpoint_interval > kMaxCheckpointInterval) {
        return Result<StreamHeader>::Err(ErrorCode::CorruptFrame, "Header checkpoint_interval out of bounds");
    }
    hdr.flags = read_u32_le(p + 12);
    hdr.stream_id = read_u64_le(p + 16);
    offset += kStreamHeaderSize;
    return Result<StreamHeader>::Ok(hdr);
}

Status encode_reference_event(const MarketEvent& ev, uint16_t schema_version, uint8_t* dst, size_t dst_len) noexcept {
    const size_t rec_sz = reference_record_size(schema_version);
    if (rec_sz == 0) {
        return Status::Error(ErrorCode::UnsupportedSchemaVersion, "Unsupported schema version");
    }
    if (dst == nullptr || dst_len < rec_sz) {
        return Status::Error(ErrorCode::BufferTooSmall, "Destination buffer smaller than reference record size");
    }
    const Status v = validate_event(ev, schema_version);
    if (!v.ok()) {
        return v;
    }

    write_u64_le(dst + 0, ev.seq_id);
    write_u64_le(dst + 8, ev.timestamp_ns);
    write_u32_le(dst + 16, ev.symbol_id);
    write_u64_le(dst + 20, ev.order_id);
    write_i64_le(dst + 28, ev.price_ticks);
    write_i64_le(dst + 36, ev.quantity);
    dst[44] = pack_meta(ev.type, ev.side, ev.scope);
    write_u16_le(dst + 45, ev.level_index);

    if (schema_version == kSchemaVersionV2) {
        write_u16_le(dst + 47, ev.venue_id);
        write_u16_le(dst + 49, ev.flags);
        write_u64_le(dst + 51, ev.match_id);
    }
    return Status::Ok();
}

Status decode_reference_event(const uint8_t* src, size_t src_len, uint16_t schema_version, MarketEvent& out) noexcept {
    const size_t rec_sz = reference_record_size(schema_version);
    if (rec_sz == 0) {
        return Status::Error(ErrorCode::UnsupportedSchemaVersion, "Unsupported schema version");
    }
    if (src == nullptr || src_len < rec_sz) {
        return Status::Error(ErrorCode::TruncatedInput, "Source buffer smaller than reference record size");
    }

    MarketEvent ev{};
    ev.seq_id = read_u64_le(src + 0);
    ev.timestamp_ns = read_u64_le(src + 8);
    ev.symbol_id = read_u32_le(src + 16);
    ev.order_id = read_u64_le(src + 20);
    ev.price_ticks = read_i64_le(src + 28);
    ev.quantity = read_i64_le(src + 36);
    if (!unpack_meta(src[44], ev.type, ev.side, ev.scope)) {
        return Status::Error(ErrorCode::CorruptFrame, "Invalid packed metadata byte in reference record");
    }
    ev.level_index = read_u16_le(src + 45);

    if (schema_version == kSchemaVersionV2) {
        ev.venue_id = read_u16_le(src + 47);
        ev.flags = read_u16_le(src + 49);
        ev.match_id = read_u64_le(src + 51);
    } else {
        // Schema v1 -> v2 upgrade defaults
        ev.venue_id = 0;
        ev.flags = 0;
        ev.match_id = 0;
    }

    const Status v = validate_event(ev, schema_version);
    if (!v.ok()) {
        return v;
    }
    out = ev;
    return Status::Ok();
}

Result<std::vector<uint8_t>> ReferenceCodec::encode_stream(
    const std::vector<MarketEvent>& events,
    const CodecConfig& config,
    DiagnosticCounters* counters) {
    const Status cfg_status = validate_config(config);
    if (!cfg_status.ok()) {
        return Result<std::vector<uint8_t>>::Err(cfg_status);
    }

    DiagnosticCounters local_counters{};
    const size_t rec_sz = reference_record_size(config.schema_version);
    const size_t batch_size = config.delta_frame_events;

    std::vector<uint8_t> out;
    out.reserve(kStreamHeaderSize + events.size() * rec_sz + ((events.size() / batch_size) + 1) * (kFrameHeaderSize + kFrameCrcSize));

    StreamHeader hdr{};
    hdr.magic = kStreamMagic;
    hdr.schema_version = config.schema_version;
    hdr.codec = CodecKind::ReferenceBinary;
    hdr.price_decimals = config.price_decimals;
    hdr.checkpoint_interval = config.checkpoint_interval;
    hdr.flags = 0;
    hdr.stream_id = config.stream_id;

    const Status hdr_st = encode_stream_header(hdr, out);
    if (!hdr_st.ok()) {
        return Result<std::vector<uint8_t>>::Err(hdr_st);
    }

    bool has_prev = false;
    uint64_t prev_seq = 0;

    size_t idx = 0;
    while (idx < events.size()) {
        const size_t count = std::min(batch_size, events.size() - idx);
        const size_t payload_bytes = count * rec_sz;
        if (payload_bytes > kMaxFramePayloadBytes) {
            return Result<std::vector<uint8_t>>::Err(ErrorCode::FrameLimitExceeded, "Reference frame exceeds max payload size");
        }

        const size_t frame_start = out.size();
        out.resize(frame_start + kFrameHeaderSize + payload_bytes + kFrameCrcSize);

        uint8_t* fh = out.data() + frame_start;
        write_u16_le(fh + 0, kFrameMagic);
        fh[2] = static_cast<uint8_t>(FrameType::ReferenceBatch);
        fh[3] = 0;
        write_u32_le(fh + 4, static_cast<uint32_t>(payload_bytes));
        write_u32_le(fh + 8, static_cast<uint32_t>(count));
        write_u64_le(fh + 12, events[idx].seq_id);

        uint8_t* payload = fh + kFrameHeaderSize;
        for (size_t i = 0; i < count; ++i) {
            const MarketEvent& ev = events[idx + i];
            const Status ev_st = validate_event(ev, config.schema_version);
            if (!ev_st.ok()) {
                return Result<std::vector<uint8_t>>::Err(ev_st);
            }
            if (has_prev) {
                if (ev.seq_id <= prev_seq) {
                    return Result<std::vector<uint8_t>>::Err(ErrorCode::SequenceRegression,
                                                             "Non-increasing seq_id in reference encoder");
                }
                if (ev.seq_id > prev_seq + 1) {
                    local_counters.sequence_gaps_detected++;
                    local_counters.missing_events_estimated += (ev.seq_id - prev_seq - 1);
                    if (config.gap_policy == GapPolicy::StrictReject) {
                        return Result<std::vector<uint8_t>>::Err(ErrorCode::SequenceGap,
                                                                 "Sequence gap detected during reference encode");
                    }
                }
            }
            prev_seq = ev.seq_id;
            has_prev = true;

            const Status enc_st = encode_reference_event(ev, config.schema_version, payload + i * rec_sz, rec_sz);
            if (!enc_st.ok()) {
                return Result<std::vector<uint8_t>>::Err(enc_st);
            }
            local_counters.events_processed++;
        }

        const uint32_t crc = crc32c(fh, kFrameHeaderSize + payload_bytes);
        write_u32_le(fh + kFrameHeaderSize + payload_bytes, crc);

        local_counters.frames_processed++;
        idx += count;
    }

    local_counters.bytes_processed = out.size();
    if (counters != nullptr) {
        *counters = local_counters;
    }
    return Result<std::vector<uint8_t>>::Ok(std::move(out));
}

Result<std::vector<MarketEvent>> ReferenceCodec::decode_stream(
    const uint8_t* data,
    size_t len,
    GapPolicy gap_policy,
    DiagnosticCounters* counters,
    StreamHeader* out_header) {
    DiagnosticCounters local_counters{};
    size_t offset = 0;
    auto hdr_res = decode_stream_header(data, len, offset);
    if (!hdr_res.ok()) {
        return Result<std::vector<MarketEvent>>::Err(hdr_res.status);
    }
    const StreamHeader hdr = *hdr_res.value;
    if (hdr.codec != CodecKind::ReferenceBinary) {
        return Result<std::vector<MarketEvent>>::Err(ErrorCode::UnsupportedCodec,
                                                     "Expected ReferenceBinary stream");
    }
    if (out_header != nullptr) {
        *out_header = hdr;
    }

    const size_t rec_sz = reference_record_size(hdr.schema_version);
    std::vector<MarketEvent> events;
    bool has_prev = false;
    uint64_t prev_seq = 0;

    while (offset < len) {
        if (offset + kFrameHeaderSize + kFrameCrcSize > len) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::TruncatedInput, "Reference frame header truncated");
        }
        const uint8_t* fh = data + offset;
        const uint16_t fmagic = read_u16_le(fh + 0);
        if (fmagic != kFrameMagic) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::InvalidMagic, "Invalid frame magic");
        }
        const uint8_t ftype = fh[2];
        if (ftype != static_cast<uint8_t>(FrameType::ReferenceBatch)) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::CorruptFrame, "Unexpected frame type in reference stream");
        }
        const uint32_t payload_len = read_u32_le(fh + 4);
        const uint32_t event_count = read_u32_le(fh + 8);
        if (payload_len > kMaxFramePayloadBytes || event_count > kMaxEventsPerFrame) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::FrameLimitExceeded, "Reference frame exceeds limits");
        }
        if (static_cast<size_t>(event_count) * rec_sz != payload_len) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::CorruptFrame, "Reference frame payload size mismatch");
        }
        const size_t total_frame_bytes = kFrameHeaderSize + static_cast<size_t>(payload_len) + kFrameCrcSize;
        if (offset + total_frame_bytes > len) {
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::TruncatedInput, "Reference frame payload truncated");
        }

        const uint32_t expected_crc = read_u32_le(fh + kFrameHeaderSize + payload_len);
        const uint32_t actual_crc = crc32c(fh, kFrameHeaderSize + payload_len);
        if (actual_crc != expected_crc) {
            local_counters.crc_errors++;
            if (counters != nullptr) {
                *counters = local_counters;
            }
            return Result<std::vector<MarketEvent>>::Err(ErrorCode::CrcMismatch, "Reference frame CRC32C mismatch");
        }

        const uint8_t* payload = fh + kFrameHeaderSize;
        events.reserve(events.size() + event_count);
        for (uint32_t i = 0; i < event_count; ++i) {
            MarketEvent ev{};
            const Status st = decode_reference_event(payload + i * rec_sz, rec_sz, hdr.schema_version, ev);
            if (!st.ok()) {
                return Result<std::vector<MarketEvent>>::Err(st);
            }
            if (hdr.schema_version == kSchemaVersionV1) {
                local_counters.schema_v1_upgrades++;
            }
            if (has_prev) {
                if (ev.seq_id <= prev_seq) {
                    return Result<std::vector<MarketEvent>>::Err(ErrorCode::SequenceRegression,
                                                                 "Sequence regression in reference stream");
                }
                if (ev.seq_id > prev_seq + 1) {
                    local_counters.sequence_gaps_detected++;
                    local_counters.missing_events_estimated += (ev.seq_id - prev_seq - 1);
                    if (gap_policy == GapPolicy::StrictReject) {
                        if (counters != nullptr) {
                            *counters = local_counters;
                        }
                        return Result<std::vector<MarketEvent>>::Err(ErrorCode::SequenceGap,
                                                                     "Sequence gap detected during reference decode");
                    }
                }
            }
            prev_seq = ev.seq_id;
            has_prev = true;
            events.push_back(ev);
            local_counters.events_processed++;
        }

        local_counters.frames_processed++;
        offset += total_frame_bytes;
    }

    local_counters.bytes_processed = offset;
    if (counters != nullptr) {
        *counters = local_counters;
    }
    return Result<std::vector<MarketEvent>>::Ok(std::move(events));
}

}  // namespace obce
