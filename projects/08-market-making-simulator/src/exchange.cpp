#include "mmsim/exchange.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

SimulatedExchange::SimulatedExchange(const ExchangeConfig& exch_cfg, const FeeConfig& fee_cfg,
                                     double tick_size_dollars)
    : exch_cfg_(exch_cfg),
      fee_cfg_(fee_cfg),
      tick_size_dollars_(tick_size_dollars),
      rng_(exch_cfg.rng_seed) {}

void SimulatedExchange::reset() {
    rng_.seed(exch_cfg_.rng_seed);
    orders_.clear();
    order_insertion_order_.clear();
    stats_ = ExchangeStats{};
    last_trade_ts_ = -1;
    last_trade_price_ = kInvalidPrice;
    last_trade_qty_remaining_ = 0;
}

double SimulatedExchange::compute_fee(PriceTicks fill_price_ticks, Qty fill_qty,
                                      LiquidityFlag liq) const noexcept {
    const double notional =
        static_cast<double>(fill_price_ticks) * tick_size_dollars_ * static_cast<double>(fill_qty);
    const double bps = (liq == LiquidityFlag::Maker) ? fee_cfg_.maker_fee_bps : fee_cfg_.taker_fee_bps;
    return notional * bps * 1e-4;
}

void SimulatedExchange::emit_fill(TimestampNs now_ns, ExchangeOrder& ord,
                                  PriceTicks fill_price_ticks, Qty fill_qty, LiquidityFlag liq,
                                  double mid_ticks, std::vector<FillEvent>& out_fills) {
    fill_qty = std::min(fill_qty, ord.leaves_qty);
    if (fill_qty <= 0) {
        return;
    }
    ord.leaves_qty -= fill_qty;
    if (liq == LiquidityFlag::Maker) {
        ++stats_.maker_fills;
    } else {
        ++stats_.taker_fills;
    }

    FillEvent f{};
    f.order_id = ord.order_id;
    f.exchange_time_ns = now_ns;
    f.side = ord.side;
    f.fill_price_ticks = fill_price_ticks;
    f.fill_qty = fill_qty;
    f.remaining_qty = ord.leaves_qty;
    f.liquidity = liq;
    f.mid_at_fill_ticks = mid_ticks;
    f.fee_dollars = compute_fee(fill_price_ticks, fill_qty, liq);
    f.raced_cancel = false;
    out_fills.push_back(f);
}

OrderAck SimulatedExchange::on_new_order(TimestampNs now_ns, const OrderRequest& req,
                                         const IL2Book& book, std::vector<FillEvent>& out_fills) {
    ++stats_.orders_received;

    OrderAck ack{};
    ack.order_id = req.order_id;
    ack.exchange_time_ns = now_ns;

    if (req.qty <= 0 || req.price_ticks <= 0 || orders_.count(req.order_id) > 0) {
        ack.accepted = false;
        ack.reject_reason = RejectReason::InvalidOrder;
        ++stats_.orders_rejected;
        return ack;
    }

    const TopOfBook tob = book.top_of_book();
    const bool crosses_bid = (req.side == Side::Bid && tob.best_ask != kInvalidPrice &&
                              req.price_ticks >= tob.best_ask);
    const bool crosses_ask = (req.side == Side::Ask && tob.best_bid != kInvalidPrice &&
                              req.price_ticks <= tob.best_bid);
    const bool crosses = crosses_bid || crosses_ask;

    if (crosses && req.post_only) {
        ack.accepted = false;
        ack.reject_reason = RejectReason::CrossesBookPostOnly;
        ++stats_.orders_rejected;
        return ack;
    }

    ++stats_.orders_accepted;
    ack.accepted = true;
    ack.reject_reason = RejectReason::None;

    ExchangeOrder ord{};
    ord.order_id = req.order_id;
    ord.side = req.side;
    ord.price_ticks = req.price_ticks;
    ord.orig_qty = req.qty;
    ord.leaves_qty = req.qty;
    ord.accept_time_ns = now_ns;

    // Handle immediate aggressive fill if non-post-only order crosses the top of book.
    if (crosses && tob.valid()) {
        const PriceTicks exec_px = (req.side == Side::Bid) ? tob.best_ask : tob.best_bid;
        const Qty avail = (req.side == Side::Bid) ? tob.ask_qty : tob.bid_qty;
        const Qty fill_q = std::min(ord.leaves_qty, avail);
        emit_fill(now_ns, ord, exec_px, fill_q, LiquidityFlag::Taker, tob.mid_ticks(), out_fills);
    }

    // Rest any remaining quantity at the back of the visible queue at our price level.
    if (ord.leaves_qty > 0) {
        ord.queue_ahead = book.level_qty(req.side, req.price_ticks);
        ack.queue_ahead = ord.queue_ahead;
        orders_.emplace(ord.order_id, ord);
        order_insertion_order_.push_back(ord.order_id);
    }

    return ack;
}

CancelAck SimulatedExchange::on_cancel(TimestampNs now_ns, const CancelRequest& req) {
    ++stats_.cancels_received;

    CancelAck ack{};
    ack.order_id = req.order_id;
    ack.exchange_time_ns = now_ns;

    auto it = orders_.find(req.order_id);
    if (it == orders_.end()) {
        ack.cancelled = false;
        ack.reject_reason = RejectReason::TooLateToCancel;
        ++stats_.cancels_too_late;
        return ack;
    }

    orders_.erase(it);
    ack.cancelled = true;
    ack.reject_reason = RejectReason::None;
    ++stats_.cancels_succeeded;
    return ack;
}

void SimulatedExchange::on_book_delta(TimestampNs now_ns, const MarketEvent& ev, Qty old_qty,
                                      const IL2Book& book, std::vector<FillEvent>& out_fills) {
    if (orders_.empty()) {
        return;
    }
    const TopOfBook tob = book.top_of_book();
    const double mid = tob.valid() ? tob.mid_ticks() : static_cast<double>(ev.price_ticks);

    // Determine how much of a level shrink is non-trade cancellation vs. the trade just processed.
    Qty cancel_drop = 0;
    if (ev.qty < old_qty) {
        Qty drop = old_qty - ev.qty;
        if (now_ns == last_trade_ts_ && ev.side == last_trade_passive_side_ &&
            ev.price_ticks == last_trade_price_ && last_trade_qty_remaining_ > 0) {
            const Qty matched = std::min(drop, last_trade_qty_remaining_);
            drop -= matched;
            last_trade_qty_remaining_ -= matched;
        }
        cancel_drop = drop;
    }

    std::vector<OrderId> finished;
    for (OrderId id : order_insertion_order_) {
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            continue;
        }
        ExchangeOrder& ord = it->second;

        // 1. Check if opposite side of market book moved strictly through our resting quote.
        const bool crossed_through =
            (ord.side == Side::Bid && tob.best_ask != kInvalidPrice && tob.best_ask < ord.price_ticks) ||
            (ord.side == Side::Ask && tob.best_bid != kInvalidPrice && tob.best_bid > ord.price_ticks);
        if (crossed_through) {
            emit_fill(now_ns, ord, ord.price_ticks, ord.leaves_qty, LiquidityFlag::Maker, mid,
                      out_fills);
            if (ord.leaves_qty == 0) {
                finished.push_back(id);
            }
            continue;
        }

        // 2. Queue-priority depletion when cancels shrink our resting level.
        if (exch_cfg_.fill_model == FillModelType::QueuePriority && ord.side == ev.side &&
            ord.price_ticks == ev.price_ticks) {
            if (cancel_drop > 0) {
                const Qty ahead_cancel = static_cast<Qty>(
                    std::llround(static_cast<double>(cancel_drop) * exch_cfg_.queue_cancel_ahead_fraction));
                ord.queue_ahead = std::max<Qty>(0, ord.queue_ahead - ahead_cancel);
            }
            // Queue ahead of us cannot exceed total visible market depth at this level.
            ord.queue_ahead = std::min(ord.queue_ahead, ev.qty);
        }
    }

    for (OrderId id : finished) {
        orders_.erase(id);
    }
    if (!finished.empty()) {
        std::erase_if(order_insertion_order_,
                      [&](OrderId id) { return orders_.find(id) == orders_.end(); });
    }
}

void SimulatedExchange::on_trade(TimestampNs now_ns, const MarketEvent& ev, const IL2Book& book,
                                 std::vector<FillEvent>& out_fills) {
    if (orders_.empty() || ev.qty <= 0) {
        return;
    }

    const Side passive_side = opposite_side(ev.side);
    last_trade_ts_ = now_ns;
    last_trade_passive_side_ = passive_side;
    last_trade_price_ = ev.price_ticks;
    last_trade_qty_remaining_ = ev.qty;

    const TopOfBook tob = book.top_of_book();
    const double mid = tob.valid() ? tob.mid_ticks() : static_cast<double>(ev.price_ticks);

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<OrderId> finished;

    for (OrderId id : order_insertion_order_) {
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            continue;
        }
        ExchangeOrder& ord = it->second;
        if (ord.side != passive_side) {
            continue;
        }

        const bool trade_through =
            (ord.side == Side::Bid && ev.price_ticks < ord.price_ticks) ||
            (ord.side == Side::Ask && ev.price_ticks > ord.price_ticks);
        const bool trade_at_price = (ev.price_ticks == ord.price_ticks);

        if (trade_through) {
            // Market traded strictly through our resting limit price -> full passive fill.
            emit_fill(now_ns, ord, ord.price_ticks, ord.leaves_qty, LiquidityFlag::Maker, mid,
                      out_fills);
        } else if (trade_at_price) {
            if (exch_cfg_.fill_model == FillModelType::QueuePriority) {
                if (ev.qty <= ord.queue_ahead) {
                    ord.queue_ahead -= ev.qty;
                } else {
                    const Qty excess = ev.qty - ord.queue_ahead;
                    ord.queue_ahead = 0;
                    emit_fill(now_ns, ord, ord.price_ticks, excess, LiquidityFlag::Maker, mid,
                              out_fills);
                }
            } else {
                // ProbabilisticTouch: fill probability decays monotonically with distance from BBO.
                PriceTicks dist = 0;
                if (tob.valid()) {
                    dist = (ord.side == Side::Bid) ? std::max<PriceTicks>(0, tob.best_bid - ord.price_ticks)
                                                   : std::max<PriceTicks>(0, ord.price_ticks - tob.best_ask);
                }
                const double p_fill =
                    std::clamp(exch_cfg_.touch_fill_prob *
                                   std::pow(exch_cfg_.distance_decay_per_tick, static_cast<double>(dist)),
                               0.0, 1.0);
                if (uni(rng_) < p_fill) {
                    emit_fill(now_ns, ord, ord.price_ticks, ev.qty, LiquidityFlag::Maker, mid,
                              out_fills);
                }
            }
        }

        if (ord.leaves_qty == 0) {
            finished.push_back(id);
        }
    }

    for (OrderId id : finished) {
        orders_.erase(id);
    }
    if (!finished.empty()) {
        std::erase_if(order_insertion_order_,
                      [&](OrderId id) { return orders_.find(id) == orders_.end(); });
    }
}

} // namespace mmsim
