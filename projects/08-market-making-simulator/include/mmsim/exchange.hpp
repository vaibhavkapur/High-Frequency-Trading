#pragma once

#include "mmsim/config.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/types.hpp"

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace mmsim {

struct ExchangeOrder {
    OrderId order_id = 0;
    Side side = Side::Bid;
    PriceTicks price_ticks = 0;
    Qty orig_qty = 0;
    Qty leaves_qty = 0;
    Qty queue_ahead = 0;
    TimestampNs accept_time_ns = 0;
};

struct ExchangeStats {
    uint64_t orders_received = 0;
    uint64_t orders_accepted = 0;
    uint64_t orders_rejected = 0;
    uint64_t cancels_received = 0;
    uint64_t cancels_succeeded = 0;
    uint64_t cancels_too_late = 0;
    uint64_t maker_fills = 0;
    uint64_t taker_fills = 0;
};

// Simulated matching venue with explicit queue-priority and probabilistic-touch fill models.
// Note: This is an explicit modeled execution simulator, not exchange L3 queue telemetry.
class SimulatedExchange {
public:
    SimulatedExchange(const ExchangeConfig& exch_cfg, const FeeConfig& fee_cfg,
                      double tick_size_dollars);

    void reset();

    // Processes an incoming new order at exchange timestamp `now_ns`.
    // May produce an OrderAck and (if non-post-only and marketable) immediate taker fills.
    OrderAck on_new_order(TimestampNs now_ns, const OrderRequest& req, const IL2Book& book,
                          std::vector<FillEvent>& out_fills);

    // Processes an incoming cancel request at exchange timestamp `now_ns`.
    CancelAck on_cancel(TimestampNs now_ns, const CancelRequest& req);

    // Processes a market BookDelta after it has been applied to `book` (with prior qty `old_qty`).
    void on_book_delta(TimestampNs now_ns, const MarketEvent& ev, Qty old_qty, const IL2Book& book,
                       std::vector<FillEvent>& out_fills);

    // Processes a market Trade event against resting simulated orders.
    void on_trade(TimestampNs now_ns, const MarketEvent& ev, const IL2Book& book,
                  std::vector<FillEvent>& out_fills);

    [[nodiscard]] size_t resting_order_count() const noexcept { return orders_.size(); }
    [[nodiscard]] const ExchangeStats& stats() const noexcept { return stats_; }

private:
    double compute_fee(PriceTicks fill_price_ticks, Qty fill_qty, LiquidityFlag liq) const noexcept;
    void emit_fill(TimestampNs now_ns, ExchangeOrder& ord, PriceTicks fill_price_ticks, Qty fill_qty,
                   LiquidityFlag liq, double mid_ticks, std::vector<FillEvent>& out_fills);

    ExchangeConfig exch_cfg_{};
    FeeConfig fee_cfg_{};
    double tick_size_dollars_ = 0.01;
    std::mt19937_64 rng_;

    std::unordered_map<OrderId, ExchangeOrder> orders_;
    // Insertion-ordered IDs for deterministic fill ordering.
    std::vector<OrderId> order_insertion_order_;
    ExchangeStats stats_{};

    // Tracks trade volume at the current timestamp/side/price to avoid double-counting queue
    // depletion when a Trade event is immediately followed by the corresponding BookDelta.
    TimestampNs last_trade_ts_ = -1;
    Side last_trade_passive_side_ = Side::Bid;
    PriceTicks last_trade_price_ = kInvalidPrice;
    Qty last_trade_qty_remaining_ = 0;
};

} // namespace mmsim
