#pragma once

#include <cstddef>
#include <cstdint>

namespace obce {

// CRC32C (Castagnoli, polynomial 0x82F63B78 reflected)
// Standard test vector: crc32c("123456789", 9) == 0xE3069283U
uint32_t crc32c_update(uint32_t initial_crc, const uint8_t* data, size_t len) noexcept;
uint32_t crc32c(const uint8_t* data, size_t len) noexcept;

// Little-endian wire encoding helpers (alignment-safe, platform-independent)
inline void write_u16_le(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFU);
    dst[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
}

inline void write_u32_le(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFU);
    dst[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    dst[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

inline void write_u64_le(uint8_t* dst, uint64_t v) noexcept {
    for (size_t i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((v >> (i * 8U)) & 0xFFU);
    }
}

inline void write_i64_le(uint8_t* dst, int64_t v) noexcept {
    write_u64_le(dst, static_cast<uint64_t>(v));
}

inline uint16_t read_u16_le(const uint8_t* src) noexcept {
    return static_cast<uint16_t>(src[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8U);
}

inline uint32_t read_u32_le(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8U) |
           (static_cast<uint32_t>(src[2]) << 16U) |
           (static_cast<uint32_t>(src[3]) << 24U);
}

inline uint64_t read_u64_le(const uint8_t* src) noexcept {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(src[i]) << (i * 8U);
    }
    return v;
}

inline int64_t read_i64_le(const uint8_t* src) noexcept {
    return static_cast<int64_t>(read_u64_le(src));
}

}  // namespace obce
