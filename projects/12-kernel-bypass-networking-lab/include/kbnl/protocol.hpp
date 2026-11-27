#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace kbnl {

inline constexpr uint32_t kPacketMagic = 0x4B424E4Cu; // "KBNL"
inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr size_t kPacketHeaderBytes = 36;
inline constexpr size_t kMinPacketBytes = 64;
inline constexpr size_t kMaxPacketBytes = 1472;

inline constexpr uint16_t kFlagNone = 0x0000u;
inline constexpr uint16_t kFlagWarmup = 0x0001u;
inline constexpr uint16_t kFlagEndOfStream = 0x0002u;
inline constexpr uint16_t kFlagBurst = 0x0004u;

enum class DecodeError {
    kOk = 0,
    kTruncated,
    kBadMagic,
    kUnsupportedVersion,
    kLengthMismatch,
    kBadChecksum,
    kInvalidSequence
};

std::string_view decode_error_name(DecodeError err) noexcept;

struct PacketHeader {
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t flags = kFlagNone;
    uint32_t feed_id = 1;
    uint32_t payload_len = 0;
    uint64_t seq_num = 0;
    uint64_t send_ts_ns = 0;
    uint32_t crc32 = 0;
};

struct DecodedPacket {
    PacketHeader header{};
    std::span<const uint8_t> payload{};
};

uint32_t compute_crc32(std::span<const uint8_t> data) noexcept;

// Deterministically fills payload bytes given seed and sequence number.
void fill_deterministic_payload(std::span<uint8_t> out_payload, uint64_t seed, uint64_t seq_num) noexcept;

// Encodes header + payload into out_buf (must be at least kPacketHeaderBytes + payload.size()).
// Returns total encoded bytes, or 0 on invalid size/parameters.
size_t encode_packet(
    const PacketHeader& header,
    std::span<const uint8_t> payload,
    std::span<uint8_t> out_buf) noexcept;

// Convenience helper producing a deterministic fixed-size packet buffer.
bool encode_deterministic_packet(
    uint64_t seq_num,
    uint64_t send_ts_ns,
    uint32_t feed_id,
    uint16_t flags,
    uint64_t seed,
    size_t total_packet_bytes,
    std::span<uint8_t> out_buf) noexcept;

// Validates and decodes an incoming packet buffer.
DecodeError decode_packet(std::span<const uint8_t> raw_bytes, DecodedPacket& out_packet) noexcept;

} // namespace kbnl
