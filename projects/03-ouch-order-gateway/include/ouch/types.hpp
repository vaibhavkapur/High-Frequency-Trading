// Basic value types shared by the codec, OMS, gateway and simulator.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace ouch {

// Fixed-width alphanumeric field: left-justified, space-padded on the right.
template <std::size_t N>
struct Alpha {
  std::array<char, N> c;

  constexpr Alpha() : c{} {
    for (auto& ch : c) ch = ' ';
  }
  constexpr Alpha(std::string_view s) : c{} {  // NOLINT(google-explicit-constructor)
    for (std::size_t i = 0; i < N; ++i) c[i] = i < s.size() ? s[i] : ' ';
  }
  constexpr Alpha(const char* s) : Alpha(std::string_view(s)) {}  // NOLINT

  [[nodiscard]] std::string_view raw() const { return {c.data(), N}; }
  [[nodiscard]] std::string str() const {
    std::size_t n = N;
    while (n > 0 && c[n - 1] == ' ') --n;
    return {c.data(), n};
  }
  [[nodiscard]] bool blank() const {
    for (char ch : c)
      if (ch != ' ') return false;
    return true;
  }
  friend bool operator==(const Alpha&, const Alpha&) = default;
  friend auto operator<=>(const Alpha&, const Alpha&) = default;
};

template <std::size_t N>
struct AlphaHash {
  std::size_t operator()(const Alpha<N>& a) const noexcept {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (char ch : a.c) {
      h ^= static_cast<unsigned char>(ch);
      h *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(h);
  }
};

using Token = Alpha<14>;
using Symbol = Alpha<8>;
using Firm = Alpha<4>;
using TokenHash = AlphaHash<14>;
using SymbolHash = AlphaHash<8>;

// Prices are unsigned integers with four implied decimal places.
inline constexpr std::uint32_t kPriceScale = 10'000;
inline constexpr std::uint32_t kMarketPrice = 0x7FFFFFFF;       // "market" sentinel
inline constexpr std::uint32_t kMaxLimitPrice = 1'999'999'900;  // $199,999.9900
inline constexpr std::uint32_t kMaxShares = 1'000'000;          // exclusive upper bound

inline constexpr std::uint32_t kTifIoc = 0;
inline constexpr std::uint32_t kTifMarketHours = 99998;
inline constexpr std::uint32_t kTifSystemHours = 99999;

constexpr bool is_buy(char side) { return side == 'B'; }
constexpr bool is_valid_side(char side) {
  return side == 'B' || side == 'S' || side == 'T' || side == 'E';
}

// Monotonic nanoseconds; used for latency measurement and journal timestamps.
inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

inline std::uint64_t wall_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace ouch
