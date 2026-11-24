#pragma once

#include "mmsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace mmsim {

inline constexpr size_t kBinHeaderSize = 16;
inline constexpr size_t kBinRecordSize = 40;
inline constexpr uint16_t kBinVersion = 1;

enum class DecodeStatus : uint8_t {
    Ok = 0,
    Truncated = 1,
    BadMagic = 2,
    UnsupportedVersion = 3,
    BadChecksum = 4,
    InvalidField = 5,
};

struct BinFileHeader {
    uint16_t version = kBinVersion;
    uint16_t flags = 0;
    uint32_t tick_size_micros = 10'000; // $0.01 = 10,000 micro-dollars
};

[[nodiscard]] uint32_t crc32_ieee(std::span<const uint8_t> data) noexcept;

void encode_bin_header(const BinFileHeader& hdr, std::span<uint8_t, kBinHeaderSize> out) noexcept;
[[nodiscard]] DecodeStatus decode_bin_header(std::span<const uint8_t> in,
                                             BinFileHeader& out) noexcept;

void encode_bin_record(const MarketEvent& ev, std::span<uint8_t, kBinRecordSize> out) noexcept;
[[nodiscard]] DecodeStatus decode_bin_record(std::span<const uint8_t> in,
                                             MarketEvent& out) noexcept;

[[nodiscard]] std::string decode_status_to_string(DecodeStatus s);

} // namespace mmsim
