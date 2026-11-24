#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mdf {

// Compile-time IEEE 802.3 CRC32 lookup table (polynomial 0xEDB88320 reflected).
// Constexpr initialization guarantees zero thread-safety hazards under TSan.
inline constexpr std::array<uint32_t, 256> kCrc32Table = []() constexpr {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
    }
    table[i] = c;
  }
  return table;
}();

inline uint32_t crc32_update(uint32_t crc, std::span<const uint8_t> data) noexcept {
  uint32_t c = crc ^ 0xFFFFFFFFU;
  for (uint8_t byte : data) {
    c = kCrc32Table[(c ^ byte) & 0xFFU] ^ (c >> 8U);
  }
  return c ^ 0xFFFFFFFFU;
}

inline uint32_t crc32(std::span<const uint8_t> data) noexcept {
  return crc32_update(0U, data);
}

}  // namespace mdf
