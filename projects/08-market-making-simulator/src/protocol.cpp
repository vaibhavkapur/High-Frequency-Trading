#include "mmsim/protocol.hpp"

#include <array>
#include <cstring>

namespace mmsim {

namespace {

inline constexpr std::array<uint8_t, 8> kMagic = {'M', 'M', 'B', 'I', 'N', '0', '0', '1'};

const std::array<uint32_t, 256>& crc32_table() noexcept {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void write_u16_le(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

void write_u64_le(uint8_t* p, uint64_t v) noexcept {
    for (size_t i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8u * i)) & 0xFFu);
    }
}

uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u));
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t read_u64_le(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8u * i);
    }
    return v;
}

} // namespace

uint32_t crc32_ieee(std::span<const uint8_t> data) noexcept {
    const auto& table = crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void encode_bin_header(const BinFileHeader& hdr, std::span<uint8_t, kBinHeaderSize> out) noexcept {
    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    write_u16_le(out.data() + 8, hdr.version);
    write_u16_le(out.data() + 10, hdr.flags);
    write_u32_le(out.data() + 12, hdr.tick_size_micros);
}

DecodeStatus decode_bin_header(std::span<const uint8_t> in, BinFileHeader& out) noexcept {
    if (in.size() < kBinHeaderSize) {
        return DecodeStatus::Truncated;
    }
    if (std::memcmp(in.data(), kMagic.data(), kMagic.size()) != 0) {
        return DecodeStatus::BadMagic;
    }
    const uint16_t version = read_u16_le(in.data() + 8);
    if (version != kBinVersion) {
        return DecodeStatus::UnsupportedVersion;
    }
    const uint16_t flags = read_u16_le(in.data() + 10);
    const uint32_t tick_micros = read_u32_le(in.data() + 12);
    if (tick_micros == 0) {
        return DecodeStatus::InvalidField;
    }
    out.version = version;
    out.flags = flags;
    out.tick_size_micros = tick_micros;
    return DecodeStatus::Ok;
}

void encode_bin_record(const MarketEvent& ev, std::span<uint8_t, kBinRecordSize> out) noexcept {
    write_u64_le(out.data() + 0, static_cast<uint64_t>(ev.timestamp_ns));
    write_u64_le(out.data() + 8, ev.seq_no);
    write_u64_le(out.data() + 16, static_cast<uint64_t>(ev.price_ticks));
    write_u64_le(out.data() + 24, static_cast<uint64_t>(ev.qty));
    out[32] = static_cast<uint8_t>(ev.type);
    out[33] = static_cast<uint8_t>(ev.side);
    write_u16_le(out.data() + 34, 0u);
    const uint32_t crc = crc32_ieee(out.first(36));
    write_u32_le(out.data() + 36, crc);
}

DecodeStatus decode_bin_record(std::span<const uint8_t> in, MarketEvent& out) noexcept {
    if (in.size() < kBinRecordSize) {
        return DecodeStatus::Truncated;
    }
    const uint32_t expected_crc = read_u32_le(in.data() + 36);
    const uint32_t actual_crc = crc32_ieee(in.first(36));
    if (expected_crc != actual_crc) {
        return DecodeStatus::BadChecksum;
    }

    const int64_t ts = static_cast<int64_t>(read_u64_le(in.data() + 0));
    const uint64_t seq = read_u64_le(in.data() + 8);
    const int64_t price = static_cast<int64_t>(read_u64_le(in.data() + 16));
    const int64_t qty = static_cast<int64_t>(read_u64_le(in.data() + 24));
    const uint8_t type_raw = in[32];
    const uint8_t side_raw = in[33];
    const uint16_t reserved = read_u16_le(in.data() + 34);

    if (reserved != 0 || ts < 0 || qty < 0) {
        return DecodeStatus::InvalidField;
    }
    if (type_raw < 1 || type_raw > 3 || side_raw > 1) {
        return DecodeStatus::InvalidField;
    }
    const auto type = static_cast<MarketEventType>(type_raw);
    if (type == MarketEventType::Trade && (qty <= 0 || price <= 0)) {
        return DecodeStatus::InvalidField;
    }
    if (type == MarketEventType::BookDelta && price <= 0) {
        return DecodeStatus::InvalidField;
    }

    out.timestamp_ns = ts;
    out.seq_no = seq;
    out.price_ticks = price;
    out.qty = qty;
    out.type = type;
    out.side = static_cast<Side>(side_raw);
    return DecodeStatus::Ok;
}

std::string decode_status_to_string(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok: return "OK";
    case DecodeStatus::Truncated: return "TRUNCATED";
    case DecodeStatus::BadMagic: return "BAD_MAGIC";
    case DecodeStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case DecodeStatus::BadChecksum: return "BAD_CHECKSUM";
    case DecodeStatus::InvalidField: return "INVALID_FIELD";
    }
    return "UNKNOWN";
}

} // namespace mmsim
