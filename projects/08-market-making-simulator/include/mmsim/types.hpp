#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace mmsim {

using TimestampNs = int64_t;
using PriceTicks = int64_t;
using Qty = int64_t;
using OrderId = uint64_t;
using SeqNo = uint64_t;

inline constexpr PriceTicks kInvalidPrice = std::numeric_limits<PriceTicks>::min();
inline constexpr int64_t kNsPerUs = 1'000;
inline constexpr int64_t kNsPerMs = 1'000'000;
inline constexpr int64_t kNsPerSec = 1'000'000'000;

enum class Side : uint8_t {
    Bid = 0,
    Ask = 1,
};

[[nodiscard]] constexpr Side opposite_side(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

[[nodiscard]] constexpr int side_sign(Side s) noexcept {
    return s == Side::Bid ? 1 : -1;
}

[[nodiscard]] constexpr std::string_view side_to_string(Side s) noexcept {
    return s == Side::Bid ? "BID" : "ASK";
}

enum class MarketEventType : uint8_t {
    BookDelta = 1, // Sets absolute aggregate quantity at (side, price_ticks); 0 deletes level
    Trade = 2,     // Aggressive trade execution at price_ticks; side = aggressor side
    BookClear = 3, // Resets the order book
};

struct MarketEvent {
    TimestampNs timestamp_ns = 0;
    SeqNo seq_no = 0;
    MarketEventType type = MarketEventType::BookDelta;
    Side side = Side::Bid;
    PriceTicks price_ticks = 0;
    Qty qty = 0;
};

enum class OrderStatus : uint8_t {
    PendingNew = 0,
    Live = 1,
    PendingCancel = 2,
    Filled = 3,
    Cancelled = 4,
    Rejected = 5,
};

enum class RejectReason : uint8_t {
    None = 0,
    InvalidOrder = 1,
    MaxOrderSize = 2,
    MaxPosition = 3,
    MaxNotional = 4,
    MaxDrawdown = 5,
    MaxOpenOrders = 6,
    TooLateToCancel = 7,
    UnknownOrder = 8,
    CrossesBookPostOnly = 9,
};

[[nodiscard]] constexpr std::string_view reject_reason_to_string(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None: return "NONE";
    case RejectReason::InvalidOrder: return "INVALID_ORDER";
    case RejectReason::MaxOrderSize: return "MAX_ORDER_SIZE";
    case RejectReason::MaxPosition: return "MAX_POSITION";
    case RejectReason::MaxNotional: return "MAX_NOTIONAL";
    case RejectReason::MaxDrawdown: return "MAX_DRAWDOWN";
    case RejectReason::MaxOpenOrders: return "MAX_OPEN_ORDERS";
    case RejectReason::TooLateToCancel: return "TOO_LATE_TO_CANCEL";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    case RejectReason::CrossesBookPostOnly: return "CROSSES_BOOK_POST_ONLY";
    }
    return "UNKNOWN";
}

enum class LiquidityFlag : uint8_t {
    Maker = 0,
    Taker = 1,
};

struct OrderRequest {
    OrderId order_id = 0;
    TimestampNs submit_time_ns = 0;
    Side side = Side::Bid;
    PriceTicks price_ticks = 0;
    Qty qty = 0;
    bool post_only = true;
};

struct CancelRequest {
    OrderId order_id = 0;
    TimestampNs submit_time_ns = 0;
};

struct OrderAck {
    OrderId order_id = 0;
    TimestampNs exchange_time_ns = 0;
    bool accepted = false;
    RejectReason reject_reason = RejectReason::None;
    Qty queue_ahead = 0;
};

struct CancelAck {
    OrderId order_id = 0;
    TimestampNs exchange_time_ns = 0;
    bool cancelled = false;
    RejectReason reject_reason = RejectReason::None;
};

struct FillEvent {
    OrderId order_id = 0;
    TimestampNs exchange_time_ns = 0;
    Side side = Side::Bid;
    PriceTicks fill_price_ticks = 0;
    Qty fill_qty = 0;
    Qty remaining_qty = 0;
    LiquidityFlag liquidity = LiquidityFlag::Maker;
    double mid_at_fill_ticks = 0.0;
    double fee_dollars = 0.0;
    bool raced_cancel = false; // True if a cancel was already in-flight when filled
};

struct TopOfBook {
    PriceTicks best_bid = kInvalidPrice;
    PriceTicks best_ask = kInvalidPrice;
    Qty bid_qty = 0;
    Qty ask_qty = 0;

    [[nodiscard]] bool valid() const noexcept {
        return best_bid != kInvalidPrice && best_ask != kInvalidPrice && best_bid < best_ask &&
               bid_qty > 0 && ask_qty > 0;
    }

    [[nodiscard]] double mid_ticks() const noexcept {
        return 0.5 * (static_cast<double>(best_bid) + static_cast<double>(best_ask));
    }

    [[nodiscard]] PriceTicks spread_ticks() const noexcept {
        return valid() ? (best_ask - best_bid) : 0;
    }
};

} // namespace mmsim
