#include "mmsim/risk.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

RiskManager::RiskManager(const RiskConfig& cfg) : cfg_(cfg) {
    reset(0.0);
}

void RiskManager::reset(double initial_equity_dollars) {
    stats_ = RiskStats{};
    peak_equity_dollars_ = initial_equity_dollars;
    kill_triggered_ = false;
}

bool RiskManager::update_equity(TimestampNs now_ns, double current_net_equity_dollars) {
    if (current_net_equity_dollars > peak_equity_dollars_) {
        peak_equity_dollars_ = current_net_equity_dollars;
    }
    const double drawdown = peak_equity_dollars_ - current_net_equity_dollars;
    if (!kill_triggered_ && cfg_.max_drawdown_dollars > 0.0 &&
        drawdown >= cfg_.max_drawdown_dollars) {
        kill_triggered_ = true;
        stats_.drawdown_kill_triggered = true;
        stats_.drawdown_kill_time_ns = now_ns;
    }
    return kill_triggered_;
}

RejectReason RiskManager::check_order(const OrderRequest& req, const RiskState& state) {
    ++stats_.checked_orders;

    auto reject = [&](RejectReason r) -> RejectReason {
        ++stats_.rejected_orders;
        const auto idx = static_cast<size_t>(r);
        if (idx < stats_.rejects_by_reason.size()) {
            ++stats_.rejects_by_reason[idx];
        }
        return r;
    };

    if (req.qty <= 0 || req.price_ticks <= 0) {
        return reject(RejectReason::InvalidOrder);
    }

    if (kill_triggered_) {
        return reject(RejectReason::MaxDrawdown);
    }

    if (req.qty > cfg_.max_order_size) {
        return reject(RejectReason::MaxOrderSize);
    }

    if (state.open_order_count >= cfg_.max_open_orders) {
        return reject(RejectReason::MaxOpenOrders);
    }

    // Worst-case position exposure accounting for all pending-new, live, and pending-cancel orders.
    const Qty next_open_buy = state.open_buy_qty + (req.side == Side::Bid ? req.qty : 0);
    const Qty next_open_sell = state.open_sell_qty + (req.side == Side::Ask ? req.qty : 0);
    const Qty worst_long = state.confirmed_position + next_open_buy;
    const Qty worst_short = state.confirmed_position - next_open_sell;
    const Qty worst_abs_pos = std::max(std::abs(worst_long), std::abs(worst_short));

    if (req.side == Side::Bid && worst_long > cfg_.max_abs_position) {
        return reject(RejectReason::MaxPosition);
    }
    if (req.side == Side::Ask && -worst_short > cfg_.max_abs_position) {
        return reject(RejectReason::MaxPosition);
    }

    if (cfg_.max_gross_notional_dollars > 0.0 && state.mark_price_dollars > 0.0) {
        const double worst_notional = static_cast<double>(worst_abs_pos) * state.mark_price_dollars;
        if (worst_notional > cfg_.max_gross_notional_dollars) {
            return reject(RejectReason::MaxNotional);
        }
    }

    ++stats_.approved_orders;
    return RejectReason::None;
}

} // namespace mmsim
