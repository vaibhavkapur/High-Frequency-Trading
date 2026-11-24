#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "sor/types.hpp"

namespace sor {

enum class QuoteUpdateType : std::uint8_t {
    Snapshot = 0,     // Full side snapshot (replaces all levels on `side`)
    UpsertLevel = 1,  // Insert or update level; quantity == 0 deletes the price level
    ClearBook = 2,    // Clear both sides for this venue
};

struct NormalizedQuoteUpdate {
    VenueId venue_id{kInvalidVenue};
    std::uint32_t symbol_id{0};
    std::uint64_t seq{0};
    TimestampNs exchange_ts_ns{0};
    TimestampNs recv_ts_ns{0};
    QuoteUpdateType type{QuoteUpdateType::UpsertLevel};
    Side side{Side::Buy};
    std::uint8_t level_count{0};
    std::array<PriceLevel, kMaxBookDepth> levels{};
};

// Binary wire format for normalized L2 quote packets (used for deterministic replay & fuzz testing).
// Layout (little-endian, packed via explicit memcpy):
//   [0..3]   uint32 magic = 0x51524F53 ("SORQ")
//   [4]      uint8  version = 1
//   [5]      uint8  venue_id
//   [6]      uint8  update_type (0=Snapshot, 1=UpsertLevel, 2=ClearBook)
//   [7]      uint8  side (0=Buy, 1=Sell)
//   [8..11]  uint32 symbol_id
//   [12..19] uint64 seq
//   [20..27] int64  exchange_ts_ns
//   [28..35] int64  recv_ts_ns
//   [36]     uint8  level_count (<= kMaxBookDepth)
//   [37..39] uint8[3] reserved (0)
//   Total header: 40 bytes.
//   Followed by `level_count` records of 20 bytes each:
//     int64  price_ticks
//     int64  quantity
//     uint32 order_count
//   Followed by 4-byte FNV-1a checksum over header + levels.
inline constexpr std::uint32_t kWireQuoteMagic = 0x51524F53U;
inline constexpr std::uint8_t kWireQuoteVersion = 1;
inline constexpr std::size_t kWireHeaderBytes = 40;
inline constexpr std::size_t kWireLevelBytes = 20;
inline constexpr std::size_t kWireChecksumBytes = 4;

enum class DecodeError : std::uint8_t {
    Ok = 0,
    Truncated = 1,
    BadMagic = 2,
    UnsupportedVersion = 3,
    InvalidEnum = 4,
    InvalidVenue = 5,
    TooManyLevels = 6,
    LengthMismatch = 7,
    ChecksumMismatch = 8,
    InvalidLevelData = 9,
};

struct DecodeResult {
    DecodeError error{DecodeError::Ok};
    NormalizedQuoteUpdate update{};
    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::Ok; }
};

struct NormalizerStats {
    std::uint64_t messages_accepted{0};
    std::uint64_t messages_rejected{0};
    std::uint64_t sequence_gaps{0};
    std::uint64_t out_of_order_dropped{0};
    std::uint64_t negative_latency_clamped{0};
};

// Encode a NormalizedQuoteUpdate into the binary wire format.
[[nodiscard]] std::vector<std::uint8_t> encode_wire_quote(const NormalizedQuoteUpdate& u);

// Decode a binary wire quote frame with strict bounds, value, and checksum validation.
[[nodiscard]] DecodeResult decode_wire_quote(std::span<const std::uint8_t> bytes) noexcept;

// Per-venue quote normalizer with sequence-gap detection and timestamp validation.
class QuoteNormalizer {
public:
    QuoteNormalizer() = default;

    // Register a venue with optional raw-to-canonical price multiplier (default 1.0).
    void register_venue(VenueId id, double price_scale_multiplier = 1.0) noexcept;

    // Validate and normalize an incoming quote update. Returns false if dropped.
    [[nodiscard]] bool normalize(const NormalizedQuoteUpdate& raw,
                                 NormalizedQuoteUpdate& out) noexcept;

    // Decode binary frame and normalize in one step.
    [[nodiscard]] DecodeError decode_and_normalize(std::span<const std::uint8_t> frame,
                                                   NormalizedQuoteUpdate& out) noexcept;

    [[nodiscard]] const NormalizerStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept;

private:
    struct VenueState {
        bool registered{false};
        double price_multiplier{1.0};
        std::uint64_t last_seq{0};
        bool has_seq{false};
    };

    std::array<VenueState, kMaxVenues> venues_{};
    NormalizerStats stats_{};
};

}  // namespace sor
