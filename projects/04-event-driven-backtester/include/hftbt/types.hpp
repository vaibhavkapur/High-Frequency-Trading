// Fundamental value types shared by every component.
#pragma once

#include <cstdint>
#include <limits>

namespace hftbt {

using Timestamp = std::int64_t; // nanoseconds, logical simulation time
using Price = std::int64_t;     // integer ticks
using Qty = std::int64_t;       // integer lots
using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;

inline constexpr Timestamp kMaxTimestamp = std::numeric_limits<Timestamp>::max();
inline constexpr Price kMaxPrice = std::numeric_limits<Price>::max() / 4;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr int sign(Side s) noexcept { return s == Side::Buy ? 1 : -1; }

// True when price `a` is strictly more aggressive than `b` for an order on `s`.
constexpr bool better(Side s, Price a, Price b) noexcept { return s == Side::Buy ? a > b : a < b; }

} // namespace hftbt
