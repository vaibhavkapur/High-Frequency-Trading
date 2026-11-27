#include "obce/crc32.hpp"

#include <array>

namespace obce {

namespace {

constexpr uint32_t kCrc32cPoly = 0x82F63B78U;

constexpr std::array<uint32_t, 256> make_crc32c_table() noexcept {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ kCrc32cPoly;
            } else {
                crc >>= 1U;
            }
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

}  // namespace

uint32_t crc32c_update(uint32_t initial_crc, const uint8_t* data, size_t len) noexcept {
    uint32_t crc = ~initial_crc;
    for (size_t i = 0; i < len; ++i) {
        crc = kCrc32cTable[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

uint32_t crc32c(const uint8_t* data, size_t len) noexcept {
    return crc32c_update(0U, data, len);
}

}  // namespace obce
