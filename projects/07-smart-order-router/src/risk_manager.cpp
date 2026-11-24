#include "sor/risk_manager.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sor {

std::string_view to_string(RiskRejectReason r) noexcept {
    switch (r) {
        case RiskRejectReason::Approved: return "APPROVED";
        case RiskRejectReason::KillSwitchActive: return "KILL_SWITCH_ACTIVE";
        case RiskRejectReason::InvalidOrder: return "INVALID_ORDER";
        case RiskRejectReason::DuplicateClientOrderId: return "DUPLICATE_CLIENT_ORDER_ID";
        case RiskRejectReason::MaxOrderQtyExceeded: return "MAX_ORDER_QTY_EXCEEDED";
        case RiskRejectReason::MaxOrderNotionalExceeded: return "MAX_ORDER_NOTIONAL_EXCEEDED";
        case RiskRejectReason::MaxNetPositionExceeded: return "MAX_NET_POSITION_EXCEEDED";
        case RiskRejectReason::MaxGrossPositionExceeded: return "MAX_GROSS_POSITION_EXCEEDED";
        case RiskRejectReason::MaxOpenNotionalExceeded: return "MAX_OPEN_NOTIONAL_EXCEEDED";
        case RiskRejectReason::OrderRateLimitExceeded: return "ORDER_RATE_LIMIT_EXCEEDED";
        case RiskRejectReason::PriceCollarBreached: return "PRICE_COLLAR_BREACHED";
    }
    return "UNKNOWN";
}

bool RiskManager::is_duplicate_client_id(const std::string& client_order_id) const {
    return !client_order_id.empty() && seen_client_ids_.contains(client_order_id);
}

RiskCheckResult RiskManager::check_and_reserve(const PreTradeOrderView& order) {
    if (limits_.kill_switch_active) {
        return {false, RiskRejectReason::KillSwitchActive, "global kill switch is active"};
    }
    if (order.qty <= 0 || order.limit_price <= 0) {
        return {false, RiskRejectReason::InvalidOrder, "order quantity and limit price must be > 0"};
    }
    if (!order.client_order_id.empty() && seen_client_ids_.contains(order.client_order_id)) {
        return {false, RiskRejectReason::DuplicateClientOrderId,
                "duplicate client_order_id: " + order.client_order_id};
    }

    // Order rate limit over sliding 1-second window.
    constexpr TimestampNs kOneSecondNs = 1'000'000'000;
    while (!recent_order_ts_.empty() && (order.now_ns - recent_order_ts_.front()) >= kOneSecondNs) {
        recent_order_ts_.pop_front();
    }
    if (limits_.max_orders_per_second > 0 &&
        recent_order_ts_.size() >= limits_.max_orders_per_second) {
        return {false, RiskRejectReason::OrderRateLimitExceeded,
                "order submission rate limit exceeded"};
    }

    if (order.qty > limits_.max_order_qty) {
        std::ostringstream oss;
        oss << "order qty " << order.qty << " exceeds max_order_qty " << limits_.max_order_qty;
        return {false, RiskRejectReason::MaxOrderQtyExceeded, oss.str()};
    }

    const double order_notional = ticks_to_usd(order.limit_price) * static_cast<double>(order.qty);
    if (order_notional > limits_.max_order_notional_usd) {
        std::ostringstream oss;
        oss << "order notional $" << order_notional << " exceeds limit $"
            << limits_.max_order_notional_usd;
        return {false, RiskRejectReason::MaxOrderNotionalExceeded, oss.str()};
    }

    // Price collar check against consolidated reference mid (if available).
    if (limits_.max_price_collar_bps > 0.0 && order.reference_mid_ticks > 0.0) {
        const double dev_bps =
            std::abs(static_cast<double>(order.limit_price) - order.reference_mid_ticks) /
            order.reference_mid_ticks * 10'000.0;
        if (dev_bps > limits_.max_price_collar_bps) {
            std::ostringstream oss;
            oss << "limit price deviates " << dev_bps << " bps from mid (collar "
                << limits_.max_price_collar_bps << " bps)";
            return {false, RiskRejectReason::PriceCollarBreached, oss.str()};
        }
    }

    // Worst-case net position check including all open working orders on the same side.
    const Quantity projected_buy = open_buy_qty_ + (order.side == Side::Buy ? order.qty : 0);
    const Quantity projected_sell = open_sell_qty_ + (order.side == Side::Sell ? order.qty : 0);
    const Quantity worst_long = std::abs(net_position_ + projected_buy);
    const Quantity worst_short = std::abs(net_position_ - projected_sell);
    if (std::max(worst_long, worst_short) > limits_.max_net_position_qty) {
        std::ostringstream oss;
        oss << "projected worst-case net position " << std::max(worst_long, worst_short)
            << " exceeds max_net_position_qty " << limits_.max_net_position_qty;
        return {false, RiskRejectReason::MaxNetPositionExceeded, oss.str()};
    }

    // Gross exposure check: |net_position| + all open buy/sell working quantity.
    const Quantity projected_gross = std::abs(net_position_) + projected_buy + projected_sell;
    if (projected_gross > limits_.max_gross_position_qty) {
        std::ostringstream oss;
        oss << "projected gross exposure " << projected_gross
            << " exceeds max_gross_position_qty " << limits_.max_gross_position_qty;
        return {false, RiskRejectReason::MaxGrossPositionExceeded, oss.str()};
    }

    if (open_notional_usd_ + order_notional > limits_.max_open_notional_usd) {
        std::ostringstream oss;
        oss << "projected open notional $" << (open_notional_usd_ + order_notional)
            << " exceeds max_open_notional_usd $" << limits_.max_open_notional_usd;
        return {false, RiskRejectReason::MaxOpenNotionalExceeded, oss.str()};
    }

    // Approved: reserve risk budget and record client order ID.
    if (!order.client_order_id.empty()) {
        seen_client_ids_.insert(order.client_order_id);
    }
    recent_order_ts_.push_back(order.now_ns);
    if (order.side == Side::Buy) {
        open_buy_qty_ += order.qty;
    } else {
        open_sell_qty_ += order.qty;
    }
    open_notional_usd_ += order_notional;

    return {true, RiskRejectReason::Approved, "ok"};
}

void RiskManager::on_fill(Side side, Quantity fill_qty, PriceTicks /*fill_price*/,
                          PriceTicks reserved_limit_price) noexcept {
    if (fill_qty <= 0) return;
    if (side == Side::Buy) {
        net_position_ += fill_qty;
        open_buy_qty_ = std::max<Quantity>(0, open_buy_qty_ - fill_qty);
    } else {
        net_position_ -= fill_qty;
        open_sell_qty_ = std::max<Quantity>(0, open_sell_qty_ - fill_qty);
    }
    const double released_notional =
        ticks_to_usd(reserved_limit_price) * static_cast<double>(fill_qty);
    open_notional_usd_ = std::max(0.0, open_notional_usd_ - released_notional);
}

void RiskManager::on_release(Side side, Quantity unreleased_qty,
                             PriceTicks reserved_limit_price) noexcept {
    if (unreleased_qty <= 0) return;
    if (side == Side::Buy) {
        open_buy_qty_ = std::max<Quantity>(0, open_buy_qty_ - unreleased_qty);
    } else {
        open_sell_qty_ = std::max<Quantity>(0, open_sell_qty_ - unreleased_qty);
    }
    const double released_notional =
        ticks_to_usd(reserved_limit_price) * static_cast<double>(unreleased_qty);
    open_notional_usd_ = std::max(0.0, open_notional_usd_ - released_notional);
}

void RiskManager::reset() {
    net_position_ = 0;
    open_buy_qty_ = 0;
    open_sell_qty_ = 0;
    open_notional_usd_ = 0.0;
    recent_order_ts_.clear();
    seen_client_ids_.clear();
}

}  // namespace sor
