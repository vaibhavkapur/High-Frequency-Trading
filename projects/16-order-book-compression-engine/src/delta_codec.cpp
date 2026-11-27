#include "obce/delta_codec.hpp"

#include <algorithm>
#include <cstring>

#include "obce/crc32.hpp"
#include "obce/reference_codec.hpp"
#include "obce/varint.hpp"

namespace obce {

namespace {

// Index footer trailer magic ("OIDX") appended at EOF when write_index_footer=true
constexpr uint32_t kIndexFooterTrailerMagic = 0x5844494FU;
constexpr size_t kIndexFooterTrailerSize = 12;  // uint64_t footer_offset + uint32_t magic
constexpr uint32_t kStreamFlagHasIndexFooter = 0x00000001U;

// Core field presence bitmask bits for delta records
constexpr uint8_t kMaskSeqNonUnit = 1U << 0U;
constexpr uint8_t kMaskTsChanged = 1U << 1U;
constexpr uint8_t kMaskSymbolChanged = 1U << 2U;
constexpr uint8_t kMaskOrderChanged = 1U << 3U;
constexpr uint8_t kMaskPriceChanged = 1U << 4U;
constexpr uint8_t kMaskQtyChanged = 1U << 5U;
constexpr uint8_t kMaskMetaChanged = 1U << 6U;
constexpr uint8_t kMaskV2ExtChanged = 1U << 7U;

// Schema v2 extension bitmask
constexpr uint8_t kExtVenueChanged = 1U << 0U;
constexpr uint8_t kExtFlagsChanged = 1U << 1U;
constexpr uint8_t kExtMatchIdChanged = 1U << 2U;

void encode_delta_event(const MarketEvent& ev,
                        const MarketEvent& prev,
                        uint16_t schema_version,
                        std::vector<uint8_t>& payload) {
    uint8_t mask = 0;

    const uint64_t seq_delta = unsigned_delta_wrap(ev.seq_id, prev.seq_id);
    if (seq_delta != 1ULL) {
        mask |= kMaskSeqNonUnit;
    }
    const uint64_t ts_delta_u = unsigned_delta_wrap(ev.timestamp_ns, prev.timestamp_ns);
    if (ts_delta_u != 0ULL) {
        mask |= kMaskTsChanged;
    }
    if (ev.symbol_id != prev.symbol_id) {
        mask |= kMaskSymbolChanged;
    }
    if (ev.order_id != prev.order_id) {
        mask |= kMaskOrderChanged;
    }
    if (ev.price_ticks != prev.price_ticks) {
        mask |= kMaskPriceChanged;
    }
    if (ev.quantity != prev.quantity) {
        mask |= kMaskQtyChanged;
    }
    const bool meta_changed = (ev.type != prev.type) || (ev.side != prev.side) ||
                              (ev.scope != prev.scope) || (ev.level_index != prev.level_index);
    if (meta_changed) {
        mask |= kMaskMetaChanged;
    }

    uint8_t ext_mask = 0;
    if (schema_version == kSchemaVersionV2) {
        if (ev.venue_id != prev.venue_id) ext_mask |= kExtVenueChanged;
        if (ev.flags != prev.flags) ext_mask |= kExtFlagsChanged;
        if (ev.match_id != prev.match_id) ext_mask |= kExtMatchIdChanged;
        if (ext_mask != 0U) {
            mask |= kMaskV2ExtChanged;
        }
    }

    payload.push_back(mask);

    if ((mask & kMaskSeqNonUnit) != 0U) {
        // Use wrap-safe signed zigzag for seq delta (though seq_id is increasing, handles full 64-bit jump cleanly)
        append_varint_u64(payload, seq_delta);
    }
    if ((mask & kMaskTsChanged) != 0U) {
        append_varint_i64(payload, static_cast<int64_t>(ts_delta_u));
    }
    if ((mask & kMaskSymbolChanged) != 0U) {
        const int64_t sym_delta = static_cast<int64_t>(ev.symbol_id) - static_cast<int64_t>(prev.symbol_id);
        append_varint_i64(payload, sym_delta);
    }
    if ((mask & kMaskOrderChanged) != 0U) {
        const uint64_t oid_delta = unsigned_delta_wrap(ev.order_id, prev.order_id);
        append_varint_i64(payload, static_cast<int64_t>(oid_delta));
    }
    if ((mask & kMaskPriceChanged) != 0U) {
        const uint64_t px_delta = signed_delta_wrap(ev.price_ticks, prev.price_ticks);
        append_varint_i64(payload, static_cast<int64_t>(px_delta));
    }
    if ((mask & kMaskQtyChanged) != 0U) {
        const uint64_t qty_delta = signed_delta_wrap(ev.quantity, prev.quantity);
        append_varint_i64(payload, static_cast<int64_t>(qty_delta));
    }
    if ((mask & kMaskMetaChanged) != 0U) {
        const uint8_t t = static_cast<uint8_t>(ev.type) & 0x0FU;
        const uint8_t s = static_cast<uint8_t>(ev.side) & 0x03U;
        const uint8_t sc = (ev.scope == BookScope::L3) ? 1U : 0U;
        const uint8_t has_lvl = (ev.level_index != prev.level_index) ? 1U : 0U;
        payload.push_back(static_cast<uint8_t>(t | (s << 4U) | (sc << 6U) | (has_lvl << 7U)));
        if (has_lvl != 0U) {
            append_varint_u64(payload, ev.level_index);
        }
    }
    if ((mask & kMaskV2ExtChanged) != 0U) {
        payload.push_back(ext_mask);
        if ((ext_mask & kExtVenueChanged) != 0U) {
            append_varint_u64(payload, ev.venue_id);
        }
        if ((ext_mask & kExtFlagsChanged) != 0U) {
            append_varint_u64(payload, ev.flags);
        }
        if ((ext_mask & kExtMatchIdChanged) != 0U) {
            const uint64_t mid_delta = unsigned_delta_wrap(ev.match_id, prev.match_id);
            append_varint_i64(payload, static_cast<int64_t>(mid_delta));
        }
    }
}

Status decode_delta_event(const uint8_t* payload,
                          size_t payload_len,
                          size_t& pos,
                          const MarketEvent& prev,
                          uint16_t schema_version,
                          MarketEvent& out) {
    if (pos >= payload_len) {
        return Status::Error(ErrorCode::TruncatedInput, "Delta event mask truncated");
    }
    const uint8_t mask = payload[pos++];
    MarketEvent ev = prev;

    if ((mask & kMaskSeqNonUnit) != 0U) {
        uint64_t seq_delta = 0;
        const ErrorCode err = decode_varint_u64(payload, payload_len, pos, seq_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode seq_id delta");
        }
        ev.seq_id = unsigned_apply_delta_wrap(prev.seq_id, seq_delta);
    } else {
        ev.seq_id = prev.seq_id + 1ULL;
    }

    if ((mask & kMaskTsChanged) != 0U) {
        int64_t ts_delta = 0;
        const ErrorCode err = decode_varint_i64(payload, payload_len, pos, ts_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode timestamp_ns delta");
        }
        ev.timestamp_ns = unsigned_apply_delta_wrap(prev.timestamp_ns, static_cast<uint64_t>(ts_delta));
    }

    if ((mask & kMaskSymbolChanged) != 0U) {
        int64_t sym_delta = 0;
        const ErrorCode err = decode_varint_i64(payload, payload_len, pos, sym_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode symbol_id delta");
        }
        const int64_t next_sym = static_cast<int64_t>(prev.symbol_id) + sym_delta;
        if (next_sym <= 0 || next_sym > static_cast<int64_t>(UINT32_MAX)) {
            return Status::Error(ErrorCode::CorruptFrame, "Decoded symbol_id out of valid uint32 range");
        }
        ev.symbol_id = static_cast<uint32_t>(next_sym);
    }

    if ((mask & kMaskOrderChanged) != 0U) {
        int64_t oid_delta = 0;
        const ErrorCode err = decode_varint_i64(payload, payload_len, pos, oid_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode order_id delta");
        }
        ev.order_id = unsigned_apply_delta_wrap(prev.order_id, static_cast<uint64_t>(oid_delta));
    }

    if ((mask & kMaskPriceChanged) != 0U) {
        int64_t px_delta = 0;
        const ErrorCode err = decode_varint_i64(payload, payload_len, pos, px_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode price_ticks delta");
        }
        ev.price_ticks = signed_apply_delta_wrap(prev.price_ticks, static_cast<uint64_t>(px_delta));
    }

    if ((mask & kMaskQtyChanged) != 0U) {
        int64_t qty_delta = 0;
        const ErrorCode err = decode_varint_i64(payload, payload_len, pos, qty_delta);
        if (err != ErrorCode::Ok) {
            return Status::Error(err, "Failed to decode quantity delta");
        }
        ev.quantity = signed_apply_delta_wrap(prev.quantity, static_cast<uint64_t>(qty_delta));
    }

    if ((mask & kMaskMetaChanged) != 0U) {
        if (pos >= payload_len) {
            return Status::Error(ErrorCode::TruncatedInput, "Delta metadata byte truncated");
        }
        const uint8_t packed = payload[pos++];
        const uint8_t t = packed & 0x0FU;
        const uint8_t s = (packed >> 4U) & 0x03U;
        const uint8_t sc = (packed >> 6U) & 0x01U;
        const uint8_t has_lvl = (packed >> 7U) & 0x01U;
        if (t < static_cast<uint8_t>(EventType::AddOrder) ||
            t > static_cast<uint8_t>(EventType::BookClear) ||
            s > static_cast<uint8_t>(Side::Ask)) {
            return Status::Error(ErrorCode::CorruptFrame, "Corrupt event type/side in delta record");
        }
        ev.type = static_cast<EventType>(t);
        ev.side = static_cast<Side>(s);
        ev.scope = (sc == 1U) ? BookScope::L3 : BookScope::L2;
        if (has_lvl != 0U) {
            uint64_t lvl = 0;
            const ErrorCode err = decode_varint_u64(payload, payload_len, pos, lvl);
            if (err != ErrorCode::Ok) {
                return Status::Error(err, "Failed to decode level_index");
            }
            if (lvl > UINT16_MAX) {
                return Status::Error(ErrorCode::CorruptFrame, "level_index exceeds uint16");
            }
            ev.level_index = static_cast<uint16_t>(lvl);
        }
    }

    if ((mask & kMaskV2ExtChanged) != 0U) {
        if (schema_version == kSchemaVersionV1) {
            return Status::Error(ErrorCode::CorruptFrame, "Schema v1 stream contains v2 extension mask");
        }
        if (pos >= payload_len) {
            return Status::Error(ErrorCode::TruncatedInput, "Delta v2 extension mask truncated");
        }
        const uint8_t ext_mask = payload[pos++];
        if ((ext_mask & 0xF8U) != 0U) {
            return Status::Error(ErrorCode::CorruptFrame, "Unknown bits set in v2 extension mask");
        }
        if ((ext_mask & kExtVenueChanged) != 0U) {
            uint64_t v = 0;
            const ErrorCode err = decode_varint_u64(payload, payload_len, pos, v);
            if (err != ErrorCode::Ok) return Status::Error(err, "Failed to decode venue_id");
            if (v > UINT16_MAX) return Status::Error(ErrorCode::CorruptFrame, "venue_id exceeds uint16");
            ev.venue_id = static_cast<uint16_t>(v);
        }
        if ((ext_mask & kExtFlagsChanged) != 0U) {
            uint64_t f = 0;
            const ErrorCode err = decode_varint_u64(payload, payload_len, pos, f);
            if (err != ErrorCode::Ok) return Status::Error(err, "Failed to decode flags");
            if (f > UINT16_MAX) return Status::Error(ErrorCode::CorruptFrame, "flags exceeds uint16");
            ev.flags = static_cast<uint16_t>(f);
        }
        if ((ext_mask & kExtMatchIdChanged) != 0U) {
            int64_t mid_delta = 0;
            const ErrorCode err = decode_varint_i64(payload, payload_len, pos, mid_delta);
            if (err != ErrorCode::Ok) return Status::Error(err, "Failed to decode match_id delta");
            ev.match_id = unsigned_apply_delta_wrap(prev.match_id, static_cast<uint64_t>(mid_delta));
        }
    } else if (schema_version == kSchemaVersionV1) {
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

struct ParsedFrameView {
    FrameType type = FrameType::Delta;
    uint8_t flags = 0;
    uint32_t payload_len = 0;
    uint32_t event_count = 0;
    uint64_t first_seq_id = 0;
    const uint8_t* payload = nullptr;
    size_t total_bytes = 0;
};

Result<ParsedFrameView> parse_and_verify_frame(const uint8_t* data, size_t len, size_t offset) {
    if (offset + kFrameHeaderSize + kFrameCrcSize > len) {
        return Result<ParsedFrameView>::Err(ErrorCode::TruncatedInput, "Frame header truncated");
    }
    const uint8_t* fh = data + offset;
    const uint16_t magic = read_u16_le(fh + 0);
    if (magic != kFrameMagic) {
        return Result<ParsedFrameView>::Err(ErrorCode::InvalidMagic, "Invalid frame magic");
    }
    const uint8_t raw_type = fh[2];
    if (raw_type < static_cast<uint8_t>(FrameType::ReferenceBatch) ||
        raw_type > static_cast<uint8_t>(FrameType::IndexFooter)) {
        return Result<ParsedFrameView>::Err(ErrorCode::CorruptFrame, "Invalid frame type");
    }
    ParsedFrameView view;
    view.type = static_cast<FrameType>(raw_type);
    view.flags = fh[3];
    view.payload_len = read_u32_le(fh + 4);
    view.event_count = read_u32_le(fh + 8);
    view.first_seq_id = read_u64_le(fh + 12);

    if (view.payload_len > kMaxFramePayloadBytes) {
        return Result<ParsedFrameView>::Err(ErrorCode::FrameLimitExceeded, "Frame payload exceeds limit");
    }
    if (view.type != FrameType::IndexFooter && view.event_count > kMaxEventsPerFrame) {
        return Result<ParsedFrameView>::Err(ErrorCode::FrameLimitExceeded, "Frame event count exceeds limit");
    }

    view.total_bytes = kFrameHeaderSize + static_cast<size_t>(view.payload_len) + kFrameCrcSize;
    if (offset + view.total_bytes > len) {
        return Result<ParsedFrameView>::Err(ErrorCode::TruncatedInput, "Frame payload/CRC truncated");
    }

    const uint32_t expected_crc = read_u32_le(fh + kFrameHeaderSize + view.payload_len);
    const uint32_t actual_crc = crc32c(fh, kFrameHeaderSize + view.payload_len);
    if (actual_crc != expected_crc) {
        return Result<ParsedFrameView>::Err(ErrorCode::CrcMismatch, "Frame CRC32C mismatch");
    }

    view.payload = fh + kFrameHeaderSize;
    return Result<ParsedFrameView>::Ok(view);
}

// Scan forward from `start_offset` to find the next valid Checkpoint frame (verified magic + CRC32C + valid anchor event).
// Returns true and sets `found_offset` if a valid checkpoint frame is located.
bool find_next_valid_checkpoint(const uint8_t* data,
                                size_t len,
                                size_t start_offset,
                                uint16_t schema_version,
                                size_t& found_offset) {
    const size_t rec_sz = reference_record_size(schema_version);
    const size_t min_cp_bytes = kFrameHeaderSize + rec_sz + kFrameCrcSize;
    if (start_offset + min_cp_bytes > len) {
        return false;
    }

    for (size_t pos = start_offset; pos + min_cp_bytes <= len; ++pos) {
        if (data[pos] != static_cast<uint8_t>(kFrameMagic & 0xFFU) ||
            data[pos + 1] != static_cast<uint8_t>((kFrameMagic >> 8U) & 0xFFU) ||
            data[pos + 2] != static_cast<uint8_t>(FrameType::Checkpoint)) {
            continue;
        }
        auto frame_res = parse_and_verify_frame(data, len, pos);
        if (!frame_res.ok()) {
            continue;
        }
        const ParsedFrameView& f = *frame_res.value;
        if (f.type == FrameType::Checkpoint && f.event_count == 1 && f.payload_len == rec_sz) {
            MarketEvent anchor{};
            if (decode_reference_event(f.payload, f.payload_len, schema_version, anchor).ok()) {
                found_offset = pos;
                return true;
            }
        }
    }
    return false;
}

Result<std::vector<CheckpointEntry>> decode_index_footer_payload(const uint8_t* payload,
                                                                 size_t payload_len,
                                                                 uint32_t entry_count) {
    // Each entry: first_seq_id(8) + last_seq_id(8) + byte_offset(8) + event_index(4) + frame_length(4) = 32 bytes
    constexpr size_t kEntryBytes = 32;
    if (static_cast<size_t>(entry_count) * kEntryBytes != payload_len) {
        return Result<std::vector<CheckpointEntry>>::Err(ErrorCode::CorruptFrame,
                                                         "IndexFooter payload length mismatch");
    }
    std::vector<CheckpointEntry> entries;
    entries.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* p = payload + static_cast<size_t>(i) * kEntryBytes;
        CheckpointEntry e;
        e.first_seq_id = read_u64_le(p + 0);
        e.last_seq_id = read_u64_le(p + 8);
        e.byte_offset = read_u64_le(p + 16);
        e.event_index = read_u32_le(p + 24);
        e.frame_length = read_u32_le(p + 28);
        entries.push_back(e);
    }
    return Result<std::vector<CheckpointEntry>>::Ok(std::move(entries));
}

}  // namespace

// ======================== DeltaEncoder ========================

DeltaEncoder::DeltaEncoder(const CodecConfig& config) : config_(config) {}

Status DeltaEncoder::reset() {
    const Status st = validate_config(config_);
    if (!st.ok()) {
        return st;
    }

    header_.magic = kStreamMagic;
    header_.schema_version = config_.schema_version;
    header_.codec = CodecKind::DeltaVarint;
    header_.price_decimals = config_.price_decimals;
    header_.checkpoint_interval = config_.checkpoint_interval;
    header_.flags = config_.write_index_footer ? kStreamFlagHasIndexFooter : 0U;
    header_.stream_id = config_.stream_id;

    buffer_.clear();
    checkpoints_.clear();
    pending_deltas_.clear();
    counters_ = DiagnosticCounters{};
    has_last_event_ = false;
    total_events_ = 0;
    events_since_checkpoint_ = 0;
    finished_ = false;

    const Status hdr_st = encode_stream_header(header_, buffer_);
    if (!hdr_st.ok()) {
        return hdr_st;
    }
    initialized_ = true;
    return Status::Ok();
}

void DeltaEncoder::append_frame(FrameType type,
                                uint32_t event_count,
                                uint64_t first_seq,
                                const std::vector<uint8_t>& payload) {
    const size_t frame_start = buffer_.size();
    buffer_.resize(frame_start + kFrameHeaderSize + payload.size() + kFrameCrcSize);

    uint8_t* fh = buffer_.data() + frame_start;
    write_u16_le(fh + 0, kFrameMagic);
    fh[2] = static_cast<uint8_t>(type);
    fh[3] = 0;
    write_u32_le(fh + 4, static_cast<uint32_t>(payload.size()));
    write_u32_le(fh + 8, event_count);
    write_u64_le(fh + 12, first_seq);

    if (!payload.empty()) {
        std::memcpy(fh + kFrameHeaderSize, payload.data(), payload.size());
    }

    const uint32_t crc = crc32c(fh, kFrameHeaderSize + payload.size());
    write_u32_le(fh + kFrameHeaderSize + payload.size(), crc);

    counters_.frames_processed++;
}

Status DeltaEncoder::flush_checkpoint_frame(const MarketEvent& anchor_event) {
    const size_t rec_sz = reference_record_size(config_.schema_version);
    std::vector<uint8_t> payload(rec_sz);
    const Status st = encode_reference_event(anchor_event, config_.schema_version, payload.data(), rec_sz);
    if (!st.ok()) {
        return st;
    }

    CheckpointEntry entry{};
    entry.first_seq_id = anchor_event.seq_id;
    entry.last_seq_id = anchor_event.seq_id;
    entry.byte_offset = buffer_.size();
    entry.event_index = static_cast<uint32_t>(total_events_);
    entry.frame_length = static_cast<uint32_t>(kFrameHeaderSize + payload.size() + kFrameCrcSize);

    append_frame(FrameType::Checkpoint, 1, anchor_event.seq_id, payload);
    checkpoints_.push_back(entry);
    counters_.checkpoint_frames++;
    return Status::Ok();
}

Status DeltaEncoder::flush_delta_frame() {
    if (pending_deltas_.empty()) {
        return Status::Ok();
    }

    std::vector<uint8_t> payload;
    payload.reserve(pending_deltas_.size() * 14U);

    MarketEvent prev = delta_base_event_;
    for (const auto& ev : pending_deltas_) {
        encode_delta_event(ev, prev, config_.schema_version, payload);
        prev = ev;
    }

    if (payload.size() > kMaxFramePayloadBytes) {
        return Status::Error(ErrorCode::FrameLimitExceeded, "Delta frame payload exceeded max limit");
    }

    append_frame(FrameType::Delta,
                 static_cast<uint32_t>(pending_deltas_.size()),
                 pending_deltas_.front().seq_id,
                 payload);

    if (!checkpoints_.empty()) {
        checkpoints_.back().last_seq_id = pending_deltas_.back().seq_id;
    }

    delta_base_event_ = pending_deltas_.back();
    pending_deltas_.clear();
    counters_.delta_frames++;
    return Status::Ok();
}

Status DeltaEncoder::append_index_footer() {
    constexpr size_t kEntryBytes = 32;
    std::vector<uint8_t> payload(checkpoints_.size() * kEntryBytes);
    for (size_t i = 0; i < checkpoints_.size(); ++i) {
        uint8_t* p = payload.data() + i * kEntryBytes;
        write_u64_le(p + 0, checkpoints_[i].first_seq_id);
        write_u64_le(p + 8, checkpoints_[i].last_seq_id);
        write_u64_le(p + 16, checkpoints_[i].byte_offset);
        write_u32_le(p + 24, checkpoints_[i].event_index);
        write_u32_le(p + 28, checkpoints_[i].frame_length);
    }

    const uint64_t footer_offset = buffer_.size();
    append_frame(FrameType::IndexFooter,
                 static_cast<uint32_t>(checkpoints_.size()),
                 footer_offset,
                 payload);

    const size_t trailer_start = buffer_.size();
    buffer_.resize(trailer_start + kIndexFooterTrailerSize);
    write_u64_le(buffer_.data() + trailer_start, footer_offset);
    write_u32_le(buffer_.data() + trailer_start + 8, kIndexFooterTrailerMagic);
    return Status::Ok();
}

Status DeltaEncoder::push(const MarketEvent& event) {
    if (!initialized_) {
        const Status init_st = reset();
        if (!init_st.ok()) {
            return init_st;
        }
    }
    if (finished_) {
        return Status::Error(ErrorCode::InvalidArgument, "Encoder already finished; call reset() before reuse");
    }

    const Status ev_st = validate_event(event, config_.schema_version);
    if (!ev_st.ok()) {
        return ev_st;
    }

    bool force_checkpoint = false;
    if (has_last_event_) {
        if (event.seq_id <= last_event_.seq_id) {
            return Status::Error(ErrorCode::SequenceRegression,
                                 "Sequence regression: seq_id " + std::to_string(event.seq_id) +
                                     " <= previous " + std::to_string(last_event_.seq_id));
        }
        if (event.seq_id > last_event_.seq_id + 1ULL) {
            counters_.sequence_gaps_detected++;
            counters_.missing_events_estimated += (event.seq_id - last_event_.seq_id - 1ULL);
            if (config_.gap_policy == GapPolicy::StrictReject) {
                return Status::Error(ErrorCode::SequenceGap,
                                     "Sequence gap: expected " + std::to_string(last_event_.seq_id + 1ULL) +
                                         ", got " + std::to_string(event.seq_id));
            }
            if (config_.gap_policy == GapPolicy::ResyncAtCheckpoint) {
                // Force a fresh checkpoint on sequence gap for clean recovery boundaries
                force_checkpoint = true;
            }
        }
    }

    const bool need_checkpoint = !has_last_event_ ||
                                 force_checkpoint ||
                                 (events_since_checkpoint_ >= config_.checkpoint_interval);

    if (need_checkpoint) {
        const Status f_st = flush_delta_frame();
        if (!f_st.ok()) {
            return f_st;
        }
        const Status cp_st = flush_checkpoint_frame(event);
        if (!cp_st.ok()) {
            return cp_st;
        }
        delta_base_event_ = event;
        events_since_checkpoint_ = 1;
    } else {
        pending_deltas_.push_back(event);
        events_since_checkpoint_++;
        if (pending_deltas_.size() >= config_.delta_frame_events) {
            const Status f_st = flush_delta_frame();
            if (!f_st.ok()) {
                return f_st;
            }
        }
    }

    last_event_ = event;
    has_last_event_ = true;
    total_events_++;
    counters_.events_processed++;
    return Status::Ok();
}

Result<EncodedStream> DeltaEncoder::finish() {
    if (!initialized_) {
        const Status init_st = reset();
        if (!init_st.ok()) {
            return Result<EncodedStream>::Err(init_st);
        }
    }
    if (finished_) {
        return Result<EncodedStream>::Err(ErrorCode::InvalidArgument, "Encoder already finished");
    }

    const Status f_st = flush_delta_frame();
    if (!f_st.ok()) {
        return Result<EncodedStream>::Err(f_st);
    }
    if (config_.write_index_footer) {
        const Status idx_st = append_index_footer();
        if (!idx_st.ok()) {
            return Result<EncodedStream>::Err(idx_st);
        }
    }

    finished_ = true;
    counters_.bytes_processed = buffer_.size();

    EncodedStream result;
    result.header = header_;
    result.bytes = std::move(buffer_);
    result.checkpoints = std::move(checkpoints_);
    result.counters = counters_;
    return Result<EncodedStream>::Ok(std::move(result));
}

Result<EncodedStream> DeltaEncoder::encode_all(
    const std::vector<MarketEvent>& events,
    const CodecConfig& config) {
    DeltaEncoder enc(config);
    const Status st = enc.reset();
    if (!st.ok()) {
        return Result<EncodedStream>::Err(st);
    }
    for (const auto& ev : events) {
        const Status push_st = enc.push(ev);
        if (!push_st.ok()) {
            return Result<EncodedStream>::Err(push_st);
        }
    }
    return enc.finish();
}

// ======================== DeltaDecoder ========================

Result<DecodedStream> DeltaDecoder::decode_all(
    const uint8_t* data,
    size_t len,
    GapPolicy gap_policy) {
    size_t offset = 0;
    auto hdr_res = decode_stream_header(data, len, offset);
    if (!hdr_res.ok()) {
        return Result<DecodedStream>::Err(hdr_res.status);
    }
    const StreamHeader hdr = *hdr_res.value;
    if (hdr.codec != CodecKind::DeltaVarint) {
        return Result<DecodedStream>::Err(ErrorCode::UnsupportedCodec,
                                                     "Expected DeltaVarint stream");
    }

    // Check if an IndexFooter trailer is present at EOF so we stop cleanly after the IndexFooter frame
    size_t frame_stream_end = len;
    if (len >= kStreamHeaderSize + kIndexFooterTrailerSize) {
        const uint32_t trailer_magic = read_u32_le(data + len - 4);
        if (trailer_magic == kIndexFooterTrailerMagic) {
            const uint64_t footer_offset = read_u64_le(data + len - kIndexFooterTrailerSize);
            if (footer_offset >= kStreamHeaderSize && footer_offset < len - kIndexFooterTrailerSize) {
                frame_stream_end = len - kIndexFooterTrailerSize;
            }
        }
    }

    DecodedStream decoded;
    decoded.header = hdr;
    DiagnosticCounters& counters = decoded.counters;

    bool has_base = false;
    bool needs_resync = false;
    MarketEvent prev_event{};

    while (offset < frame_stream_end) {
        auto frame_res = parse_and_verify_frame(data, frame_stream_end, offset);
        if (!frame_res.ok()) {
            if (frame_res.status.code == ErrorCode::CrcMismatch) {
                counters.crc_errors++;
            }
            if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                counters.corrupt_frames_skipped++;
                size_t next_cp = 0;
                if (find_next_valid_checkpoint(data, frame_stream_end, offset + 1, hdr.schema_version, next_cp)) {
                    counters.resync_recoveries++;
                    offset = next_cp;
                    needs_resync = false;
                    has_base = false;
                    continue;
                }
                // No further checkpoint to resync to; stop gracefully if we already decoded events,
                // or return the error if nothing could be recovered.
                if (!decoded.events.empty()) {
                    break;
                }
            }
            return Result<DecodedStream>::Err(frame_res.status);
        }

        const ParsedFrameView& f = *frame_res.value;
        const size_t next_offset = offset + f.total_bytes;

        if (f.type == FrameType::IndexFooter) {
            auto idx_res = decode_index_footer_payload(f.payload, f.payload_len, f.event_count);
            if (!idx_res.ok()) {
                if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                    counters.corrupt_frames_skipped++;
                    offset = next_offset;
                    continue;
                }
                return Result<DecodedStream>::Err(idx_res.status);
            }
            if (decoded.checkpoints.empty()) {
                decoded.checkpoints = std::move(*idx_res.value);
            }
            counters.frames_processed++;
            offset = next_offset;
            continue;
        }

        if (f.type == FrameType::Checkpoint) {
            if (f.event_count != 1) {
                if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                    counters.corrupt_frames_skipped++;
                    needs_resync = true;
                    offset = next_offset;
                    continue;
                }
                return Result<DecodedStream>::Err(ErrorCode::CorruptFrame,
                                                  "Checkpoint frame must contain exactly 1 anchor event");
            }
            MarketEvent anchor{};
            const Status st = decode_reference_event(f.payload, f.payload_len, hdr.schema_version, anchor);
            if (!st.ok()) {
                if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                    counters.corrupt_frames_skipped++;
                    needs_resync = true;
                    offset = next_offset;
                    continue;
                }
                return Result<DecodedStream>::Err(st);
            }

            if (has_base && !needs_resync) {
                if (anchor.seq_id <= prev_event.seq_id) {
                    return Result<DecodedStream>::Err(ErrorCode::SequenceRegression,
                                                      "Sequence regression at checkpoint frame");
                }
                if (anchor.seq_id > prev_event.seq_id + 1ULL) {
                    counters.sequence_gaps_detected++;
                    counters.missing_events_estimated += (anchor.seq_id - prev_event.seq_id - 1ULL);
                    if (gap_policy == GapPolicy::StrictReject) {
                        return Result<DecodedStream>::Err(ErrorCode::SequenceGap,
                                                          "Sequence gap detected at checkpoint frame");
                    }
                }
            } else if (!decoded.events.empty() && anchor.seq_id > decoded.events.back().seq_id + 1ULL) {
                counters.sequence_gaps_detected++;
                counters.missing_events_estimated += (anchor.seq_id - decoded.events.back().seq_id - 1ULL);
            }

            CheckpointEntry cp{};
            cp.first_seq_id = anchor.seq_id;
            cp.last_seq_id = anchor.seq_id;
            cp.byte_offset = offset;
            cp.event_index = static_cast<uint32_t>(decoded.events.size());
            cp.frame_length = static_cast<uint32_t>(f.total_bytes);
            decoded.checkpoints.push_back(cp);

            if (hdr.schema_version == kSchemaVersionV1) {
                counters.schema_v1_upgrades++;
            }
            decoded.events.push_back(anchor);
            prev_event = anchor;
            has_base = true;
            needs_resync = false;
            counters.checkpoint_frames++;
            counters.frames_processed++;
            counters.events_processed++;
            offset = next_offset;
            continue;
        }

        if (f.type == FrameType::Delta) {
            if (!has_base || needs_resync) {
                // Cannot decode delta without valid base state!
                if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                    counters.corrupt_frames_skipped++;
                    offset = next_offset;
                    continue;
                }
                return Result<DecodedStream>::Err(ErrorCode::CorruptFrame,
                                                  "Delta frame encountered without valid preceding checkpoint");
            }

            // Detect a missing preceding delta frame!
            // A delta frame's first_seq_id must match what follows prev_event when no frame was dropped.
            // If f.first_seq_id > prev_event.seq_id + 1, we check whether the first delta event itself encodes
            // the gap (which happens under AllowAndFlag) or whether a whole delta frame was dropped in transit!
            // If a whole delta frame was dropped, prev_event is stale, so applying the delta would corrupt prices/order_ids.
            // We detect dropped delta frame by checking decoded first_ev.seq_id vs header's f.first_seq_id!
            size_t pos = 0;
            std::vector<MarketEvent> frame_events;
            frame_events.reserve(f.event_count);
            MarketEvent running = prev_event;
            Status frame_err = Status::Ok();
            bool dropped_delta_detected = false;

            for (uint32_t i = 0; i < f.event_count; ++i) {
                MarketEvent ev{};
                const Status dst = decode_delta_event(f.payload, f.payload_len, pos, running, hdr.schema_version, ev);
                if (!dst.ok()) {
                    frame_err = dst;
                    break;
                }
                if (i == 0 && ev.seq_id != f.first_seq_id) {
                    // Header's authoritative first_seq_id does not match prev_event.seq_id + delta!
                    // This definitively proves an intermediate delta frame was lost!
                    dropped_delta_detected = true;
                    frame_err = Status::Error(
                        ErrorCode::SequenceGap,
                        "Missing preceding delta frame detected: frame header first_seq_id=" +
                            std::to_string(f.first_seq_id) + ", computed seq_id=" + std::to_string(ev.seq_id));
                    break;
                }
                if (ev.seq_id <= running.seq_id) {
                    frame_err = Status::Error(ErrorCode::SequenceRegression, "Sequence regression inside delta frame");
                    break;
                }
                if (ev.seq_id > running.seq_id + 1ULL) {
                    counters.sequence_gaps_detected++;
                    counters.missing_events_estimated += (ev.seq_id - running.seq_id - 1ULL);
                    if (gap_policy == GapPolicy::StrictReject) {
                        frame_err = Status::Error(ErrorCode::SequenceGap, "Sequence gap inside delta frame");
                        break;
                    }
                }
                running = ev;
                frame_events.push_back(ev);
            }

            if (frame_err.ok() && pos != f.payload_len) {
                frame_err = Status::Error(ErrorCode::CorruptFrame, "Trailing unconsumed bytes in delta frame");
            }

            if (!frame_err.ok()) {
                if (dropped_delta_detected) {
                    counters.sequence_gaps_detected++;
                    if (f.first_seq_id > prev_event.seq_id + 1ULL) {
                        counters.missing_events_estimated += (f.first_seq_id - prev_event.seq_id - 1ULL);
                    }
                }
                if (gap_policy == GapPolicy::ResyncAtCheckpoint) {
                    counters.corrupt_frames_skipped++;
                    needs_resync = true;
                    size_t next_cp = 0;
                    if (find_next_valid_checkpoint(data, frame_stream_end, next_offset, hdr.schema_version, next_cp)) {
                        counters.resync_recoveries++;
                        offset = next_cp;
                        needs_resync = false;
                        has_base = false;
                        continue;
                    }
                    // No more checkpoints left
                    break;
                }
                return Result<DecodedStream>::Err(frame_err);
            }

            for (const auto& ev : frame_events) {
                if (hdr.schema_version == kSchemaVersionV1) {
                    counters.schema_v1_upgrades++;
                }
                decoded.events.push_back(ev);
                counters.events_processed++;
            }
            if (!decoded.checkpoints.empty() && !frame_events.empty()) {
                decoded.checkpoints.back().last_seq_id = frame_events.back().seq_id;
            }
            prev_event = running;
            counters.delta_frames++;
            counters.frames_processed++;
            offset = next_offset;
            continue;
        }

        return Result<DecodedStream>::Err(ErrorCode::CorruptFrame, "Unsupported frame type in delta stream");
    }

    counters.bytes_processed = len;
    return Result<DecodedStream>::Ok(std::move(decoded));
}

Result<std::vector<CheckpointEntry>> DeltaDecoder::read_checkpoint_index(
    const uint8_t* data,
    size_t len,
    StreamHeader* out_header) {
    size_t offset = 0;
    auto hdr_res = decode_stream_header(data, len, offset);
    if (!hdr_res.ok()) {
        return Result<std::vector<CheckpointEntry>>::Err(hdr_res.status);
    }
    const StreamHeader hdr = *hdr_res.value;
    if (hdr.codec != CodecKind::DeltaVarint) {
        return Result<std::vector<CheckpointEntry>>::Err(ErrorCode::UnsupportedCodec,
                                                         "Checkpoint index requires DeltaVarint stream");
    }
    if (out_header != nullptr) {
        *out_header = hdr;
    }

    // Fast path: O(1) seek to IndexFooter via EOF trailer
    if (len >= kStreamHeaderSize + kIndexFooterTrailerSize) {
        const uint32_t trailer_magic = read_u32_le(data + len - 4);
        if (trailer_magic == kIndexFooterTrailerMagic) {
            const uint64_t footer_offset = read_u64_le(data + len - kIndexFooterTrailerSize);
            if (footer_offset >= kStreamHeaderSize && footer_offset < len - kIndexFooterTrailerSize) {
                auto frame_res = parse_and_verify_frame(data, len - kIndexFooterTrailerSize,
                                                        static_cast<size_t>(footer_offset));
                if (frame_res.ok() && frame_res.value->type == FrameType::IndexFooter) {
                    return decode_index_footer_payload(frame_res.value->payload,
                                                       frame_res.value->payload_len,
                                                       frame_res.value->event_count);
                }
            }
        }
    }

    // Fallback path: scan frame headers to build checkpoint index
    std::vector<CheckpointEntry> entries;
    uint32_t event_idx = 0;
    while (offset < len) {
        auto frame_res = parse_and_verify_frame(data, len, offset);
        if (!frame_res.ok()) {
            return Result<std::vector<CheckpointEntry>>::Err(frame_res.status);
        }
        const ParsedFrameView& f = *frame_res.value;
        if (f.type == FrameType::IndexFooter) {
            return decode_index_footer_payload(f.payload, f.payload_len, f.event_count);
        }
        if (f.type == FrameType::Checkpoint) {
            CheckpointEntry cp{};
            cp.first_seq_id = f.first_seq_id;
            cp.last_seq_id = f.first_seq_id;
            cp.byte_offset = offset;
            cp.event_index = event_idx;
            cp.frame_length = static_cast<uint32_t>(f.total_bytes);
            entries.push_back(cp);
            event_idx += f.event_count;
        } else if (f.type == FrameType::Delta) {
            if (!entries.empty() && f.event_count > 0) {
                entries.back().last_seq_id = std::max(entries.back().last_seq_id,
                                                      f.first_seq_id + f.event_count - 1ULL);
            }
            event_idx += f.event_count;
        }
        offset += f.total_bytes;
    }

    return Result<std::vector<CheckpointEntry>>::Ok(std::move(entries));
}

Result<MarketEvent> DeltaDecoder::seek_by_seq_id(
    const uint8_t* data,
    size_t len,
    uint64_t target_seq_id,
    const std::vector<CheckpointEntry>& index,
    const StreamHeader& header,
    DiagnosticCounters* counters) {
    auto range_res = decode_seq_range(data, len, target_seq_id, target_seq_id, index, header, counters);
    if (!range_res.ok()) {
        return Result<MarketEvent>::Err(range_res.status);
    }
    if (range_res.value->empty()) {
        return Result<MarketEvent>::Err(ErrorCode::CheckpointNotFound,
                                        "Target seq_id " + std::to_string(target_seq_id) + " not found in stream");
    }
    return Result<MarketEvent>::Ok(range_res.value->front());
}

Result<std::vector<MarketEvent>> DeltaDecoder::decode_seq_range(
    const uint8_t* data,
    size_t len,
    uint64_t start_seq_id,
    uint64_t end_seq_id,
    const std::vector<CheckpointEntry>& index,
    const StreamHeader& header,
    DiagnosticCounters* counters) {
    if (start_seq_id == 0 || end_seq_id < start_seq_id) {
        return Result<std::vector<MarketEvent>>::Err(ErrorCode::InvalidArgument, "Invalid sequence range");
    }
    if (index.empty()) {
        return Result<std::vector<MarketEvent>>::Err(ErrorCode::CheckpointNotFound, "Empty checkpoint index");
    }

    // Binary search for the last checkpoint whose first_seq_id <= start_seq_id
    auto it = std::upper_bound(index.begin(), index.end(), start_seq_id,
                               [](uint64_t seq, const CheckpointEntry& e) {
                                   return seq < e.first_seq_id;
                               });
    if (it == index.begin()) {
        // start_seq_id is before the very first checkpoint
        if (end_seq_id < index.front().first_seq_id) {
            return Result<std::vector<MarketEvent>>::Ok(std::vector<MarketEvent>{});
        }
    } else {
        --it;
    }

    size_t frame_stream_end = len;
    if (len >= kStreamHeaderSize + kIndexFooterTrailerSize &&
        read_u32_le(data + len - 4) == kIndexFooterTrailerMagic) {
        frame_stream_end = len - kIndexFooterTrailerSize;
    }

    size_t offset = static_cast<size_t>(it->byte_offset);
    if (offset >= frame_stream_end) {
        return Result<std::vector<MarketEvent>>::Err(ErrorCode::CorruptFrame, "Checkpoint offset out of bounds");
    }

    std::vector<MarketEvent> results;
    DiagnosticCounters local_counters{};
    bool has_base = false;
    MarketEvent prev{};

    while (offset < frame_stream_end) {
        auto frame_res = parse_and_verify_frame(data, frame_stream_end, offset);
        if (!frame_res.ok()) {
            return Result<std::vector<MarketEvent>>::Err(frame_res.status);
        }
        const ParsedFrameView& f = *frame_res.value;
        offset += f.total_bytes;

        if (f.type == FrameType::IndexFooter) {
            break;
        }
        if (f.type == FrameType::Checkpoint) {
            if (f.first_seq_id > end_seq_id) {
                break;
            }
            MarketEvent anchor{};
            const Status st = decode_reference_event(f.payload, f.payload_len, header.schema_version, anchor);
            if (!st.ok()) {
                return Result<std::vector<MarketEvent>>::Err(st);
            }
            prev = anchor;
            has_base = true;
            local_counters.checkpoint_frames++;
            local_counters.frames_processed++;
            local_counters.events_processed++;
            if (anchor.seq_id >= start_seq_id && anchor.seq_id <= end_seq_id) {
                results.push_back(anchor);
            }
            if (anchor.seq_id >= end_seq_id) {
                break;
            }
        } else if (f.type == FrameType::Delta) {
            if (!has_base) {
                return Result<std::vector<MarketEvent>>::Err(ErrorCode::CorruptFrame,
                                                             "Seek landed on delta without checkpoint");
            }
            if (f.first_seq_id > end_seq_id) {
                break;
            }
            size_t pos = 0;
            for (uint32_t i = 0; i < f.event_count; ++i) {
                MarketEvent ev{};
                const Status dst = decode_delta_event(f.payload, f.payload_len, pos, prev, header.schema_version, ev);
                if (!dst.ok()) {
                    return Result<std::vector<MarketEvent>>::Err(dst);
                }
                prev = ev;
                local_counters.events_processed++;
                if (ev.seq_id >= start_seq_id && ev.seq_id <= end_seq_id) {
                    results.push_back(ev);
                }
                if (ev.seq_id > end_seq_id) {
                    if (counters != nullptr) *counters = local_counters;
                    return Result<std::vector<MarketEvent>>::Ok(std::move(results));
                }
            }
            local_counters.delta_frames++;
            local_counters.frames_processed++;
        }
    }

    if (counters != nullptr) {
        *counters = local_counters;
    }
    return Result<std::vector<MarketEvent>>::Ok(std::move(results));
}

}  // namespace obce
