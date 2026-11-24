// Big-endian (network order) integer loads/stores.
//
// All loads go through individual byte reads, so they are independent of host
// endianness and alignment and never alias the buffer as a wider type.
// Compilers fold these into a single load + byte swap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace itch {

constexpr std::uint16_t load_be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((std::uint32_t{p[0]} << 8) | std::uint32_t{p[1]});
}

constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
         std::uint32_t{p[3]};
}

// ITCH timestamps are 6-byte nanoseconds-since-midnight.
constexpr std::uint64_t load_be48(const std::uint8_t* p) noexcept {
  return (std::uint64_t{p[0]} << 40) | (std::uint64_t{p[1]} << 32) | (std::uint64_t{p[2]} << 24) |
         (std::uint64_t{p[3]} << 16) | (std::uint64_t{p[4]} << 8) | std::uint64_t{p[5]};
}

constexpr std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  return (std::uint64_t{load_be32(p)} << 32) | std::uint64_t{load_be32(p + 4)};
}

// Stores the low `bytes` bytes of `v`, most significant first.
constexpr void store_be(std::uint8_t* p, std::uint64_t v, std::size_t bytes) noexcept {
  for (std::size_t i = 0; i < bytes; ++i) {
    p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
  }
}

}  // namespace itch
