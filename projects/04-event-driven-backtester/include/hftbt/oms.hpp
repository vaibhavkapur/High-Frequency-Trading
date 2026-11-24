// Strategy-side order management and pre-trade risk. Everything here is
// built from what the strategy has OBSERVED (delayed exec reports), never
// from exchange truth.
#pragma once

#include <unordered_map>
#include <vector>

#include "hftbt/order.hpp"

namespace hftbt {

struct RiskConfig {
    Qty max_order_qty = 1'000'000;
    Qty max_position = 1'000'000; // per instrument, absolute, worst case incl. open orders
    std::size_t max_open_orders = 10'000;
};

enum class OrderStatus : std::uint8_t { PendingNew, Live, PendingCancel };

struct OmsOrder {
    OrderId id = 0;
    InstrumentId instrument = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    Qty filled = 0;
    OrderStatus status = OrderStatus::PendingNew;
    Qty open() const noexcept { return qty - filled; }
};

class Oms {
public:
    Oms(std::size_t n_instruments, const RiskConfig& risk);

    // Pre-trade check; on success the order is registered as PendingNew.
    RejectReason try_register(const OrderRequest& req);
    // Marks PendingCancel. False if the order is unknown, already terminal
    // (as far as we have observed) or already has a cancel in flight.
    bool try_cancel(OrderId id);
    void on_report(const ExecReport& r);

    Qty position(InstrumentId i) const noexcept { return inst_[i].position; }
    Qty open_qty(InstrumentId i, Side s) const noexcept {
        return s == Side::Buy ? inst_[i].open_buy : inst_[i].open_sell;
    }
    std::size_t open_orders() const noexcept { return orders_.size(); }
    const OmsOrder* find(OrderId id) const {
        const auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

private:
    struct Instrument {
        Qty position = 0;
        Qty open_buy = 0;
        Qty open_sell = 0;
    };
    void close(std::unordered_map<OrderId, OmsOrder>::iterator it);

    RiskConfig risk_;
    std::vector<Instrument> inst_;
    std::unordered_map<OrderId, OmsOrder> orders_;
};

} // namespace hftbt
