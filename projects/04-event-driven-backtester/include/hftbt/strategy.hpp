// Strategy API. A strategy only ever touches the Context, which exposes the
// DELAYED world: the observed book, observed fills, and the current
// simulation time (the time the strategy is actually running at).
#pragma once

#include <string>

#include "hftbt/oms.hpp"
#include "hftbt/order.hpp"
#include "hftbt/order_book.hpp"

namespace hftbt {

struct SendResult {
    OrderId id = 0; // 0 when rejected by local pre-trade risk
    RejectReason reason = RejectReason::None;
    explicit operator bool() const noexcept { return id != 0; }
};

class Context {
public:
    virtual ~Context() = default;

    virtual Timestamp now() const = 0;
    virtual std::size_t num_instruments() const = 0;
    virtual const std::string& symbol(InstrumentId i) const = 0;
    // Book as reconstructed from market data that has reached the strategy.
    virtual const OrderBook& book(InstrumentId i) const = 0;
    // Position / order state from exec reports that have reached the strategy.
    virtual const Oms& oms() const = 0;
    Qty position(InstrumentId i) const { return oms().position(i); }

    virtual SendResult send_limit(InstrumentId i, Side side, Price price, Qty qty,
                                  Tif tif = Tif::GTC, bool post_only = false) = 0;
    virtual SendResult send_market(InstrumentId i, Side side, Qty qty) = 0;
    virtual bool cancel(OrderId id) = 0;
    // Fires on_timer at `at` (clamped to now). Returns the timer id.
    virtual std::uint64_t set_timer(Timestamp at) = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_start(Context&) {}
    // Called after the observed book has been updated with `ev`. ev.ts is the
    // exchange timestamp; ctx.now() is when the strategy sees it. For
    // C/D/E events side/price are filled in from the resting order.
    virtual void on_market_data(Context&, const MarketEvent& /*ev*/) {}
    // Called after the OMS has processed the (delayed) report.
    virtual void on_order_update(Context&, const ExecReport& /*report*/) {}
    virtual void on_timer(Context&, std::uint64_t /*timer_id*/) {}
    virtual void on_stop(Context&) {}
};

} // namespace hftbt
