#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace obce {

// Wire magic constants ("OBCE" = 0x4543424F little-endian)
inline constexpr uint32_t kStreamMagic = 0x4543424FU;
inline constexpr uint16_t kFrameMagic = 0x4F42U;  // "BO" / "OB"

// Schema versions supported by this build
inline constexpr uint16_t kSchemaVersionV1 = 1;
inline constexpr uint16_t kSchemaVersionV2 = 2;

// Default fixed-point scaling: 10^8 (e.g., 100.25 -> 10025000000)
inline constexpr uint8_t kDefaultPriceDecimals = 8;
inline constexpr int64_t kDefaultPriceScale = 100'000'000LL;

// Bounded capacity guards to protect against malformed/adversarial payloads
inline constexpr uint32_t kMaxFramePayloadBytes = 16U * 1024U * 1024U;  // 16 MiB
inline constexpr uint32_t kMaxEventsPerFrame = 65536U;
inline constexpr uint32_t kMaxCheckpointInterval = 65536U;

enum class EventType : uint8_t {
    AddOrder = 1,      // L3: add individual order
    ModifyOrder = 2,   // L3: modify price/qty of existing order
    CancelOrder = 3,   // L3: cancel/delete order
    ExecuteOrder = 4,  // L3: trade execution against resting order
    L2Update = 5,      // L2: aggregated price-level quantity update
    L2Snapshot = 6,    // L2: book level snapshot entry
    BookClear = 7      // Book reset / trading halt clear
};

enum class Side : uint8_t {
    Unknown = 0,
    Bid = 1,
    Ask = 2
};

enum class BookScope : uint8_t {
    L2 = 2,
    L3 = 3
};

enum class CodecKind : uint8_t {
    ReferenceBinary = 1,
    DeltaVarint = 2,
    DeltaVarintZstd = 3
};

enum class FrameType : uint8_t {
    ReferenceBatch = 1,
    Checkpoint = 2,
    Delta = 3,
    IndexFooter = 4
};

enum class GapPolicy : uint8_t {
    StrictReject = 0,         // Fail immediately on any sequence gap (seq != expected)
    AllowAndFlag = 1,         // Accept non-decreasing gaps, increment gap counter
    ResyncAtCheckpoint = 2    // On corrupt/missing frame during decode, skip to next valid checkpoint
};

enum class ErrorCode : uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    InvalidEvent = 2,
    BufferTooSmall = 3,
    TruncatedInput = 4,
    InvalidMagic = 5,
    UnsupportedSchemaVersion = 6,
    UnsupportedCodec = 7,
    UnsupportedFeature = 8,
    CrcMismatch = 9,
    SequenceGap = 10,
    SequenceRegression = 11,
    VarintOverflow = 12,
    FrameLimitExceeded = 13,
    CorruptFrame = 14,
    CheckpointNotFound = 15,
    IoError = 16
};

inline const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
        case ErrorCode::InvalidEvent: return "INVALID_EVENT";
        case ErrorCode::BufferTooSmall: return "BUFFER_TOO_SMALL";
        case ErrorCode::TruncatedInput: return "TRUNCATED_INPUT";
        case ErrorCode::InvalidMagic: return "INVALID_MAGIC";
        case ErrorCode::UnsupportedSchemaVersion: return "UNSUPPORTED_SCHEMA_VERSION";
        case ErrorCode::UnsupportedCodec: return "UNSUPPORTED_CODEC";
        case ErrorCode::UnsupportedFeature: return "UNSUPPORTED_FEATURE";
        case ErrorCode::CrcMismatch: return "CRC_MISMATCH";
        case ErrorCode::SequenceGap: return "SEQUENCE_GAP";
        case ErrorCode::SequenceRegression: return "SEQUENCE_REGRESSION";
        case ErrorCode::VarintOverflow: return "VARINT_OVERFLOW";
        case ErrorCode::FrameLimitExceeded: return "FRAME_LIMIT_EXCEEDED";
        case ErrorCode::CorruptFrame: return "CORRUPT_FRAME";
        case ErrorCode::CheckpointNotFound: return "CHECKPOINT_NOT_FOUND";
        case ErrorCode::IoError: return "IO_ERROR";
    }
    return "UNKNOWN";
}

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    bool ok() const noexcept { return code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    static Status Ok() { return Status{ErrorCode::Ok, {}}; }
    static Status Error(ErrorCode c, std::string msg) {
        return Status{c, std::move(msg)};
    }
};

template <typename T>
struct Result {
    Status status;
    std::optional<T> value;

    bool ok() const noexcept { return status.ok() && value.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    static Result<T> Ok(T v) {
        return Result<T>{Status::Ok(), std::move(v)};
    }
    static Result<T> Err(ErrorCode c, std::string msg) {
        return Result<T>{Status::Error(c, std::move(msg)), std::nullopt};
    }
    static Result<T> Err(Status s) {
        return Result<T>{std::move(s), std::nullopt};
    }
};

// Canonical market event model (v1 + v2 fields).
// In schema v1, v2 extension fields (venue_id, flags, match_id) default to 0.
struct MarketEvent {
    uint64_t seq_id = 0;         // Strictly monotonic sequence ID per stream (gap-detectable)
    uint64_t timestamp_ns = 0;   // Exchange/matching-engine timestamp in nanoseconds since epoch
    uint32_t symbol_id = 0;      // Compact integer instrument identifier (> 0)
    uint64_t order_id = 0;       // Order ID (L3); 0 for pure L2 updates or BookClear
    int64_t price_ticks = 0;     // Signed fixed-point price in 10^(-price_decimals) units
    int64_t quantity = 0;        // Non-negative fixed-point/lot quantity (>= 0)
    EventType type = EventType::AddOrder;
    Side side = Side::Bid;
    BookScope scope = BookScope::L3;
    uint16_t level_index = 0;    // L2 depth level index (0 = top-of-book); 0 for L3

    // Schema v2 extension fields (ignored/zeroed when encoding to v1)
    uint16_t venue_id = 0;
    uint16_t flags = 0;
    uint64_t match_id = 0;

    bool operator==(const MarketEvent& o) const noexcept {
        return seq_id == o.seq_id &&
               timestamp_ns == o.timestamp_ns &&
               symbol_id == o.symbol_id &&
               order_id == o.order_id &&
               price_ticks == o.price_ticks &&
               quantity == o.quantity &&
               type == o.type &&
               side == o.side &&
               scope == o.scope &&
               level_index == o.level_index &&
               venue_id == o.venue_id &&
               flags == o.flags &&
               match_id == o.match_id;
    }

    bool operator!=(const MarketEvent& o) const noexcept { return !(*this == o); }
};

struct StreamHeader {
    uint32_t magic = kStreamMagic;
    uint16_t schema_version = kSchemaVersionV2;
    CodecKind codec = CodecKind::DeltaVarint;
    uint8_t price_decimals = kDefaultPriceDecimals;
    uint32_t checkpoint_interval = 256;
    uint32_t flags = 0;
    uint64_t stream_id = 1;
};

struct CheckpointEntry {
    uint64_t first_seq_id = 0;
    uint64_t last_seq_id = 0;
    uint64_t byte_offset = 0;    // Offset of checkpoint frame in stream
    uint32_t event_index = 0;    // 0-based index of first event in checkpoint frame
    uint32_t frame_length = 0;   // Total serialized bytes of checkpoint frame
};

struct CodecConfig {
    uint16_t schema_version = kSchemaVersionV2;
    uint8_t price_decimals = kDefaultPriceDecimals;
    uint32_t checkpoint_interval = 256;  // Events per checkpoint cycle (1 checkpoint + up to N-1 deltas)
    uint32_t delta_frame_events = 64;    // Max events packed into each delta frame
    uint64_t stream_id = 1;
    GapPolicy gap_policy = GapPolicy::StrictReject;
    bool write_index_footer = true;      // Append seekable checkpoint index footer
};

struct DiagnosticCounters {
    uint64_t frames_processed = 0;
    uint64_t checkpoint_frames = 0;
    uint64_t delta_frames = 0;
    uint64_t events_processed = 0;
    uint64_t sequence_gaps_detected = 0;
    uint64_t missing_events_estimated = 0;
    uint64_t crc_errors = 0;
    uint64_t corrupt_frames_skipped = 0;
    uint64_t resync_recoveries = 0;
    uint64_t schema_v1_upgrades = 0;
    uint64_t bytes_processed = 0;
};

// Validate event invariants according to schema contract
Status validate_event(const MarketEvent& ev, uint16_t schema_version) noexcept;

// Validate codec configuration
Status validate_config(const CodecConfig& cfg) noexcept;

// Helper for byte-stable canonical digest (CRC32C) of a sequence of events
uint32_t compute_events_digest(const std::vector<MarketEvent>& events, uint16_t schema_version) noexcept;

}  // namespace obce
