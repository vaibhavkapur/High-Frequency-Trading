#include "fpga_ob/reference_book.hpp"

#include <limits>
#include <stdexcept>

namespace fpga_ob {

ReferenceOrderBook::ReferenceOrderBook(Config cfg) : cfg_(cfg) {
    if (auto err = cfg_.validate(); err.has_value()) {
        throw std::invalid_argument("Invalid ReferenceOrderBook config: " + *err);
    }
}

void ReferenceOrderBook::reset() {
    bids_.clear();
    asks_.clear();
}

Status ReferenceOrderBook::apply_one(const EventPacket& e) {
    if (e.schema_ver != kSchemaVersion) {
        return Status::ErrUnsupportedSchema;
    }
    if (e.flags != 0) {
        return Status::ErrUnsupportedFlags;
    }
    if (e.opcode > static_cast<uint8_t>(Opcode::Execute)) {
        return Status::ErrUnsupportedOpcode;
    }
    if (e.opcode == static_cast<uint8_t>(Opcode::Nop)) {
        return Status::Ok;
    }
    if (e.side != static_cast<uint8_t>(Side::Bid) && e.side != static_cast<uint8_t>(Side::Ask)) {
        return Status::ErrInvalidSide;
    }
    if (e.price < cfg_.price_base || e.price >= cfg_.price_base + cfg_.num_levels) {
        return Status::ErrPriceOob;
    }
    if (e.qty == 0) {
        return Status::ErrZeroQty;
    }

    auto& book = (e.side == static_cast<uint8_t>(Side::Bid)) ? bids_ : asks_;
    auto it = book.find(e.price);

    switch (static_cast<Opcode>(e.opcode)) {
        case Opcode::Add: {
            if (it == book.end()) {
                if (book.size() >= cfg_.max_active_levels) {
                    return Status::ErrFullCapacity;
                }
                book.emplace(e.price, Level{e.qty, 1});
                return Status::Ok;
            }
            uint64_t next_qty = static_cast<uint64_t>(it->second.qty) + static_cast<uint64_t>(e.qty);
            if (next_qty > std::numeric_limits<uint32_t>::max()) {
                return Status::ErrOverflowQty;
            }
            if (it->second.orders == std::numeric_limits<uint16_t>::max()) {
                return Status::ErrOverflowOrders;
            }
            it->second.qty = static_cast<uint32_t>(next_qty);
            it->second.orders = static_cast<uint16_t>(it->second.orders + 1);
            return Status::Ok;
        }
        case Opcode::Cancel: {
            if (it == book.end() || e.qty > it->second.qty) {
                return Status::ErrInvalidCancel;
            }
            uint32_t next_qty = it->second.qty - e.qty;
            if (next_qty == 0) {
                book.erase(it);
            } else {
                it->second.qty = next_qty;
                it->second.orders = (it->second.orders > 1)
                                        ? static_cast<uint16_t>(it->second.orders - 1)
                                        : uint16_t{1};
            }
            return Status::Ok;
        }
        case Opcode::Execute: {
            if (it == book.end() || e.qty > it->second.qty) {
                return Status::ErrInvalidExecute;
            }
            uint32_t next_qty = it->second.qty - e.qty;
            if (next_qty == 0) {
                book.erase(it);
            } else {
                it->second.qty = next_qty;
            }
            return Status::Ok;
        }
        case Opcode::Nop:
            return Status::Ok;
    }
    return Status::ErrUnsupportedOpcode;
}

BboOutputPacket ReferenceOrderBook::snapshot(uint32_t seq0, uint32_t seq1, bool lane1_valid,
                                             Status st0, Status st1, uint32_t ts_ns) const {
    BboOutputPacket out{};
    out.schema_ver        = kSchemaVersion;
    out.lane1_valid       = lane1_valid;
    out.status_0          = st0;
    out.status_1          = st1;
    out.seq_no_0          = seq0;
    out.seq_no_1          = lane1_valid ? seq1 : 0;
    out.timestamp_ns      = ts_ns;
    out.active_bid_levels = static_cast<uint16_t>(bids_.size());
    out.active_ask_levels = static_cast<uint16_t>(asks_.size());

    if (!bids_.empty()) {
        const auto& best = *bids_.rbegin();
        out.best_bid_valid  = true;
        out.best_bid_price  = best.first;
        out.best_bid_qty    = best.second.qty;
        out.best_bid_orders = best.second.orders;
    }
    if (!asks_.empty()) {
        const auto& best = *asks_.begin();
        out.best_ask_valid  = true;
        out.best_ask_price  = best.first;
        out.best_ask_qty    = best.second.qty;
        out.best_ask_orders = best.second.orders;
    }
    if (out.best_bid_valid && out.best_ask_valid) {
        out.spread  = static_cast<int32_t>(out.best_ask_price) - static_cast<int32_t>(out.best_bid_price);
        out.crossed = (out.best_bid_price >= out.best_ask_price);
    }
    return out;
}

BboOutputPacket ReferenceOrderBook::apply_single(const EventPacket& e) {
    Status st0 = apply_one(e);
    return snapshot(e.seq_no, 0, false, st0, Status::Ok, e.timestamp_ns);
}

BboOutputPacket ReferenceOrderBook::apply_pair(const EventPacket& e0, const EventPacket& e1) {
    Status st0 = apply_one(e0);
    Status st1 = apply_one(e1);
    return snapshot(e0.seq_no, e1.seq_no, true, st0, st1, e1.timestamp_ns);
}

}  // namespace fpga_ob
