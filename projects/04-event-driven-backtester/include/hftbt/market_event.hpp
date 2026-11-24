// Historical market-data event: the unit of the input feed.
#pragma once

#include "hftbt/types.hpp"

namespace hftbt {

// L3 (market-by-order): Add / Cancel / Delete / Execute.
// L2 (market-by-price): Level / Trade.
enum class MdType : std::uint8_t {
    Add = 'A',     // new resting order: order_id, side, price, qty
    Cancel = 'C',  // reduce resting order by qty (qty >= remaining removes it)
    Delete = 'D',  // remove resting order entirely
    Execute = 'E', // resting order order_id traded qty (aggressor is the other side)
    Level = 'L',   // absolute aggregate qty at (side, price); 0 removes the level
    Trade = 'T',   // trade print: side = AGGRESSOR side, price, qty
};

constexpr bool is_valid_md_type(std::uint8_t c) noexcept {
    return c == 'A' || c == 'C' || c == 'D' || c == 'E' || c == 'L' || c == 'T';
}

struct MarketEvent {
    Timestamp ts = 0; // exchange timestamp, ns
    OrderId order_id = 0;
    Price price = 0;
    Qty qty = 0;
    InstrumentId instrument = 0;
    MdType type = MdType::Add;
    Side side = Side::Buy;

    friend bool operator==(const MarketEvent&, const MarketEvent&) = default;
};

} // namespace hftbt
