#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obce/types.hpp"

namespace obce {

// Maximum bytes for an unsigned 64-bit LEB128 varint (ceil(64 / 7) = 10)
inline constexpr size_t kMaxVarint64Bytes = 10;

// ZigZag encoding maps signed 64-bit integers to unsigned 64-bit integers
// so small-magnitude negative numbers have small unsigned encodings.
// Safe for all int64_t values including INT64_MIN (no signed overflow UB).
inline constexpr uint64_t zigzag_encode(int64_t value) noexcept {
    const uint64_t u = static_cast<uint64_t>(value);
    return (u << 1U) ^ static_cast<uint64_t>(value >> 63);
}

inline constexpr int64_t zigzag_decode(uint64_t encoded) noexcept {
    return static_cast<int64_t>((encoded >> 1U) ^ (~(encoded & 1ULL) + 1ULL));
}

// Wrap-safe delta computation using modular 64-bit unsigned subtraction.
// Allows representing any transition between arbitrary int64_t values (even INT64_MIN <-> INT64_MAX)
// without C++ signed integer overflow UB!
inline constexpr uint64_t signed_delta_wrap(int64_t current, int64_t previous) noexcept {
    return static_cast<uint64_t>(current) - static_cast<uint64_t>(previous);
}

inline constexpr int64_t signed_apply_delta_wrap(int64_t previous, uint64_t delta) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(previous) + delta);
}

inline constexpr uint64_t unsigned_delta_wrap(uint64_t current, uint64_t previous) noexcept {
    return current - previous;
}

inline constexpr uint64_t unsigned_apply_delta_wrap(uint64_t previous, uint64_t delta) noexcept {
    return previous + delta;
}

// Encode unsigned 64-bit value as LEB128 varint into dst (must have >= 10 bytes capacity).
// Returns bytes written (1..10).
inline size_t encode_varint_u64(uint64_t value, uint8_t* dst) noexcept {
    size_t i = 0;
    while (value >= 0x80ULL) {
        dst[i++] = static_cast<uint8_t>((value & 0x7FULL) | 0x80ULL);
        value >>= 7U;
    }
    dst[i++] = static_cast<uint8_t>(value);
    return i;
}

inline void append_varint_u64(std::vector<uint8_t>& out, uint64_t value) {
    uint8_t buf[kMaxVarint64Bytes];
    const size_t n = encode_varint_u64(value, buf);
    out.insert(out.end(), buf, buf + n);
}

inline void append_varint_i64(std::vector<uint8_t>& out, int64_t value) {
    append_varint_u64(out, zigzag_encode(value));
}

// Decode LEB128 unsigned 64-bit varint from [data, data + len).
// Advances `offset` by bytes consumed on success.
// Rejects truncated varints and malformed >64-bit encodings (10th byte > 0x01).
inline ErrorCode decode_varint_u64(const uint8_t* data, size_t len, size_t& offset, uint64_t& out) noexcept {
    uint64_t result = 0;
    unsigned shift = 0;
    size_t pos = offset;

    for (size_t i = 0; i < kMaxVarint64Bytes; ++i) {
        if (pos >= len) {
            return ErrorCode::TruncatedInput;
        }
        const uint8_t byte = data[pos++];
        if (i == 9 && byte > 0x01U) {
            // 10th byte can only carry bit 63 (0x00 or 0x01) without continuation
            return ErrorCode::VarintOverflow;
        }
        result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            offset = pos;
            out = result;
            return ErrorCode::Ok;
        }
        shift += 7U;
    }
    return ErrorCode::VarintOverflow;
}

inline ErrorCode decode_varint_i64(const uint8_t* data, size_t len, size_t& offset, int64_t& out) noexcept {
    uint64_t zz = 0;
    const ErrorCode err = decode_varint_u64(data, len, offset, zz);
    if (err != ErrorCode::Ok) {
        return err;
    }
    out = zigzag_decode(zz);
    return ErrorCode::Ok;
}

}  // namespace obce
