// Core scalar types shared by every component.
//
// Prices and quantities are signed 64-bit fixed-point integers. The engine never
// touches floating point: a price is a count of "price units" (the venue decides
// what a unit is, e.g. 1e-4 of a currency unit) and each symbol declares a tick
// size in those units. Quantities work the same way with a lot size.
#pragma once

#include <bit>
#include <cstdint>

namespace me {

static_assert(std::endian::native == std::endian::little,
              "wire format is defined as little-endian; add byte swapping to port");

using Price = std::int64_t;      // fixed-point price units; must be a multiple of the tick size
using Qty = std::int64_t;        // fixed-point quantity units; signed so negatives are representable and rejectable
using OrderId = std::uint64_t;   // client-assigned, unique among live orders of one symbol
using SymbolId = std::uint32_t;  // dense index into the engine's symbol table
using Seq = std::uint64_t;       // engine-wide monotonic report sequence number, starts at 1

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

// One aggregated price level as seen by snapshots.
struct LevelView {
  Price price = 0;
  Qty qty = 0;               // sum of the remaining quantity of all orders at this price
  std::uint32_t orders = 0;  // number of live orders at this price

  friend bool operator==(const LevelView&, const LevelView&) = default;
};

// What the book knows about one resting order.
struct OrderInfo {
  Side side = Side::Buy;
  Price price = 0;
  Qty leaves = 0;

  friend bool operator==(const OrderInfo&, const OrderInfo&) = default;
};

}  // namespace me
