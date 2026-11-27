#include "kbnl/protocol.hpp"

#include <array>
#include <cstring>

namespace kbnl {

namespace {

constexpr std::array<uint32_t, 256> make_crc32_table() noexcept {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kCrc32Table = make_crc32_table();

void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

void write_be64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (56 - i * 8)) & 0xFFu);
    }
}

uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

uint32_t crc32_update(uint32_t crc, std::span<const uint8_t> data) noexcept {
    uint32_t c = crc;
    for (uint8_t b : data) {
        c = kCrc32Table[(c ^ b) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

} // namespace

std::string_view decode_error_name(DecodeError err) noexcept {
    switch (err) {
        case DecodeError::kOk: return "OK";
        case DecodeError::kTruncated: return "TRUNCATED";
        case DecodeError::kBadMagic: return "BAD_MAGIC";
        case DecodeError::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
        case DecodeError::kLengthMismatch: return "LENGTH_MISMATCH";
        case DecodeError::kBadChecksum: return "BAD_CHECKSUM";
        case DecodeError::kInvalidSequence: return "INVALID_SEQUENCE";
    }
    return "UNKNOWN";
}

uint32_t compute_crc32(std::span<const uint8_t> data) noexcept {
    return crc32_update(0xFFFFFFFFu, data) ^ 0xFFFFFFFFu;
}

void fill_deterministic_payload(std::span<uint8_t> out_payload, uint64_t seed, uint64_t seq_num) noexcept {
    uint64_t state = seed ^ (seq_num * 0x9E3779B97F4A7C15ULL);
    size_t offset = 0;
    while (offset < out_payload.size()) {
        state = splitmix64(state);
        for (int i = 0; i < 8 && offset < out_payload.size(); ++i) {
            out_payload[offset++] = static_cast<uint8_t>((state >> (i * 8)) & 0xFFu);
        }
    }
}

size_t encode_packet(
    const PacketHeader& header,
    std::span<const uint8_t> payload,
    std::span<uint8_t> out_buf) noexcept {
    const size_t total_len = kPacketHeaderBytes + payload.size();
    if (total_len < kMinPacketBytes || total_len > kMaxPacketBytes) {
        return 0;
    }
    if (out_buf.size() < total_len) {
        return 0;
    }
    if ((header.flags & kFlagEndOfStream) == 0 && header.seq_num == 0) {
        return 0;
    }

    uint8_t* dst = out_buf.data();
    write_be32(dst + 0, header.magic);
    write_be16(dst + 4, header.version);
    write_be16(dst + 6, header.flags);
    write_be32(dst + 8, header.feed_id);
    write_be32(dst + 12, static_cast<uint32_t>(payload.size()));
    write_be64(dst + 16, header.seq_num);
    write_be64(dst + 24, header.send_ts_ns);

    if (!payload.empty()) {
        std::memcpy(dst + kPacketHeaderBytes, payload.data(), payload.size());
    }

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, std::span<const uint8_t>(dst, 32));
    crc = crc32_update(crc, std::span<const uint8_t>(dst + kPacketHeaderBytes, payload.size()));
    crc ^= 0xFFFFFFFFu;
    write_be32(dst + 32, crc);

    return total_len;
}

bool encode_deterministic_packet(
    uint64_t seq_num,
    uint64_t send_ts_ns,
    uint32_t feed_id,
    uint16_t flags,
    uint64_t seed,
    size_t total_packet_bytes,
    std::span<uint8_t> out_buf) noexcept {
    if (total_packet_bytes < kMinPacketBytes || total_packet_bytes > kMaxPacketBytes) {
        return false;
    }
    if (out_buf.size() < total_packet_bytes) {
        return false;
    }
    const size_t payload_len = total_packet_bytes - kPacketHeaderBytes;
    std::array<uint8_t, kMaxPacketBytes> payload_tmp{};
    fill_deterministic_payload(std::span<uint8_t>(payload_tmp.data(), payload_len), seed, seq_num);

    PacketHeader hdr{};
    hdr.magic = kPacketMagic;
    hdr.version = kProtocolVersion;
    hdr.flags = flags;
    hdr.feed_id = feed_id;
    hdr.payload_len = static_cast<uint32_t>(payload_len);
    hdr.seq_num = seq_num;
    hdr.send_ts_ns = send_ts_ns;

    return encode_packet(hdr, std::span<const uint8_t>(payload_tmp.data(), payload_len), out_buf) == total_packet_bytes;
}

DecodeError decode_packet(std::span<const uint8_t> raw_bytes, DecodedPacket& out_packet) noexcept {
    if (raw_bytes.size() < kMinPacketBytes) {
        return DecodeError::kTruncated;
    }
    const uint8_t* src = raw_bytes.data();
    PacketHeader hdr{};
    hdr.magic = read_be32(src + 0);
    if (hdr.magic != kPacketMagic) {
        return DecodeError::kBadMagic;
    }
    hdr.version = read_be16(src + 4);
    if (hdr.version != kProtocolVersion) {
        return DecodeError::kUnsupportedVersion;
    }
    hdr.flags = read_be16(src + 6);
    hdr.feed_id = read_be32(src + 8);
    hdr.payload_len = read_be32(src + 12);
    hdr.seq_num = read_be64(src + 16);
    hdr.send_ts_ns = read_be64(src + 24);
    hdr.crc32 = read_be32(src + 32);

    const size_t expected_size = kPacketHeaderBytes + static_cast<size_t>(hdr.payload_len);
    if (expected_size != raw_bytes.size() || expected_size > kMaxPacketBytes) {
        return DecodeError::kLengthMismatch;
    }

    if ((hdr.flags & kFlagEndOfStream) == 0 && hdr.seq_num == 0) {
        return DecodeError::kInvalidSequence;
    }

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, std::span<const uint8_t>(src, 32));
    crc = crc32_update(crc, std::span<const uint8_t>(src + kPacketHeaderBytes, hdr.payload_len));
    crc ^= 0xFFFFFFFFu;

    if (crc != hdr.crc32) {
        return DecodeError::kBadChecksum;
    }

    out_packet.header = hdr;
    out_packet.payload = std::span<const uint8_t>(src + kPacketHeaderBytes, hdr.payload_len);
    return DecodeError::kOk;
}

} // namespace kbnl
