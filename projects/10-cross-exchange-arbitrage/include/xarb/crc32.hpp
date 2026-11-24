#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xarb {

// Standard IEEE 802.3 CRC32 (polynomial 0xEDB88320 reflected).
// Used by OKX order book top-25 checksum verification and binary feed frame verification.
class Crc32 {
 public:
  static uint32_t compute(const void* data, size_t len, uint32_t seed = 0) noexcept {
    const auto& tbl = table();
    uint32_t crc = ~seed;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      crc = tbl[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
  }

  static uint32_t compute(std::string_view sv, uint32_t seed = 0) noexcept {
    return compute(sv.data(), sv.size(), seed);
  }

  // OKX sends checksum as signed 32-bit integer in JSON.
  static int32_t compute_signed(std::string_view sv) noexcept {
    return static_cast<int32_t>(compute(sv));
  }

 private:
  static const std::array<uint32_t, 256>& table() noexcept {
    static const std::array<uint32_t, 256> kTable = [] {
      std::array<uint32_t, 256> t{};
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
          c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        t[i] = c;
      }
      return t;
    }();
    return kTable;
  }
};

}  // namespace xarb
