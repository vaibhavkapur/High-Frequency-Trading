#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include "fpga_ob/contracts.hpp"

namespace fpga_ob {

struct Diagnostics {
    uint64_t cycles_total           = 0;
    uint64_t reset_cycles           = 0;
    uint64_t events_accepted        = 0;
    uint64_t events_committed       = 0;
    uint64_t events_rejected        = 0;
    uint64_t backpressure_stalls    = 0;
    uint64_t dual_lane_cycles       = 0;
    uint16_t max_active_bid_levels  = 0;
    uint16_t max_active_ask_levels  = 0;

    uint64_t err_unsupported_schema = 0;
    uint64_t err_unsupported_flags  = 0;
    uint64_t err_unsupported_opcode = 0;
    uint64_t err_invalid_side       = 0;
    uint64_t err_price_oob          = 0;
    uint64_t err_zero_qty           = 0;
    uint64_t err_full_capacity      = 0;
    uint64_t err_overflow_qty       = 0;
    uint64_t err_overflow_orders    = 0;
    uint64_t err_invalid_cancel     = 0;
    uint64_t err_invalid_execute    = 0;

    void record_status(Status st) {
        switch (st) {
            case Status::Ok:                   ++events_committed; break;
            case Status::ErrUnsupportedSchema: ++events_rejected; ++err_unsupported_schema; break;
            case Status::ErrUnsupportedFlags:  ++events_rejected; ++err_unsupported_flags; break;
            case Status::ErrUnsupportedOpcode: ++events_rejected; ++err_unsupported_opcode; break;
            case Status::ErrInvalidSide:       ++events_rejected; ++err_invalid_side; break;
            case Status::ErrPriceOob:          ++events_rejected; ++err_price_oob; break;
            case Status::ErrZeroQty:           ++events_rejected; ++err_zero_qty; break;
            case Status::ErrFullCapacity:      ++events_rejected; ++err_full_capacity; break;
            case Status::ErrOverflowQty:       ++events_rejected; ++err_overflow_qty; break;
            case Status::ErrOverflowOrders:    ++events_rejected; ++err_overflow_orders; break;
            case Status::ErrInvalidCancel:     ++events_rejected; ++err_invalid_cancel; break;
            case Status::ErrInvalidExecute:    ++events_rejected; ++err_invalid_execute; break;
        }
    }

    [[nodiscard]] std::string to_json() const {
        std::ostringstream os;
        os << "{"
           << "\"cycles_total\":" << cycles_total << ","
           << "\"reset_cycles\":" << reset_cycles << ","
           << "\"events_accepted\":" << events_accepted << ","
           << "\"events_committed\":" << events_committed << ","
           << "\"events_rejected\":" << events_rejected << ","
           << "\"backpressure_stalls\":" << backpressure_stalls << ","
           << "\"dual_lane_cycles\":" << dual_lane_cycles << ","
           << "\"max_active_bid_levels\":" << max_active_bid_levels << ","
           << "\"max_active_ask_levels\":" << max_active_ask_levels << ","
           << "\"errors\":{"
           << "\"unsupported_schema\":" << err_unsupported_schema << ","
           << "\"unsupported_flags\":" << err_unsupported_flags << ","
           << "\"unsupported_opcode\":" << err_unsupported_opcode << ","
           << "\"invalid_side\":" << err_invalid_side << ","
           << "\"price_oob\":" << err_price_oob << ","
           << "\"zero_qty\":" << err_zero_qty << ","
           << "\"full_capacity\":" << err_full_capacity << ","
           << "\"overflow_qty\":" << err_overflow_qty << ","
           << "\"overflow_orders\":" << err_overflow_orders << ","
           << "\"invalid_cancel\":" << err_invalid_cancel << ","
           << "\"invalid_execute\":" << err_invalid_execute
           << "}}";
        return os.str();
    }
};

}  // namespace fpga_ob
