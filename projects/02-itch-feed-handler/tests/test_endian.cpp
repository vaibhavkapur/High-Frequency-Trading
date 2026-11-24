#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "itch/endian.hpp"

using namespace itch;

TEST_CASE("big-endian loads read most significant byte first", "[endian]") {
  const std::array<std::uint8_t, 8> b{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  CHECK(load_be16(b.data()) == 0x0123);
  CHECK(load_be32(b.data()) == 0x01234567u);
  CHECK(load_be48(b.data()) == 0x0123456789ABull);
  CHECK(load_be64(b.data()) == 0x0123456789ABCDEFull);
}

TEST_CASE("loads work at odd (unaligned) offsets", "[endian]") {
  const std::array<std::uint8_t, 9> b{0xFF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  CHECK(load_be16(b.data() + 1) == 0x0123);
  CHECK(load_be32(b.data() + 1) == 0x01234567u);
  CHECK(load_be64(b.data() + 1) == 0x0123456789ABCDEFull);
}

TEST_CASE("high bit set does not sign-extend", "[endian]") {
  const std::array<std::uint8_t, 8> b{0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};
  CHECK(load_be16(b.data()) == 0xFFFE);
  CHECK(load_be32(b.data()) == 0xFFFEFDFCu);
  CHECK(load_be48(b.data()) == 0xFFFEFDFCFBFAull);
  CHECK(load_be64(b.data()) == 0xFFFEFDFCFBFAF9F8ull);
}

TEST_CASE("store_be is the inverse of the loads", "[endian]") {
  std::array<std::uint8_t, 8> b{};
  store_be(b.data(), 0x0123, 2);
  CHECK(b[0] == 0x01);
  CHECK(b[1] == 0x23);
  store_be(b.data(), 0x0123456789ABull, 6);
  CHECK(load_be48(b.data()) == 0x0123456789ABull);
  store_be(b.data(), 0xDEADBEEFCAFEF00Dull, 8);
  CHECK(load_be64(b.data()) == 0xDEADBEEFCAFEF00Dull);
}

// The loads are constexpr, so the same facts hold at compile time.
static_assert(load_be16(std::array<std::uint8_t, 2>{0x12, 0x34}.data()) == 0x1234);
