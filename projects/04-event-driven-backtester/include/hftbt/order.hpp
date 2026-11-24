// Order-entry messages exchanged between the strategy side and the
// simulated exchange.
#pragma once

#include "hftbt/types.hpp"

namespace hftbt {

enum class Tif : std::uint8_t { GTC, IOC };
enum class OrdType : std::uint8_t { Limit, Market };

struct OrderRequest {
    OrderId id = 0;
    InstrumentId instrument = 0;
    Side side = Side::Buy;
    OrdType type = OrdType::Limit;
    Tif tif = Tif::GTC;
    bool post_only = false;
    Price price = 0;
    Qty qty = 0;
};

struct CancelRequest {
    OrderId id = 0;
    InstrumentId instrument = 0;
};

enum class ExecType : std::uint8_t { Ack, Fill, Cancelled, Rejected, CancelRejected };

enum class RejectReason : std::uint8_t {
    None,
    InvalidOrder,       // exchange: malformed request
    PostOnlyWouldCross, // exchange
    TooLateToCancel,    // exchange: order already filled / cancelled / unknown
    RiskMaxOrderQty,    // local pre-trade risk
    RiskMaxPosition,    // local pre-trade risk
    RiskMaxOpenOrders,  // local pre-trade risk
};

const char* to_string(ExecType t) noexcept;
const char* to_string(RejectReason r) noexcept;

struct ExecReport {
    Timestamp exch_ts = 0; // when it happened at the exchange (not when observed)
    OrderId order_id = 0;
    InstrumentId instrument = 0;
    ExecType type = ExecType::Ack;
    Side side = Side::Buy;
    bool maker = false; // fills only
    RejectReason reason = RejectReason::None;
    Price price = 0; // fill price (ticks, includes modelled slippage)
    Qty qty = 0;     // Fill: executed qty. Cancelled/Rejected: qty removed.
    Qty leaves = 0;  // qty still working after this report
};

} // namespace hftbt
