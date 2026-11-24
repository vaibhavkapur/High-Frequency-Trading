#include "mmsim/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

Portfolio::Portfolio(double tick_size_dollars, double initial_cash_dollars) {
    reset(tick_size_dollars, initial_cash_dollars);
}

void Portfolio::reset(double tick_size_dollars, double initial_cash_dollars) {
    tick_size_dollars_ = tick_size_dollars;
    initial_cash_dollars_ = initial_cash_dollars;
    cash_dollars_ = initial_cash_dollars;
    position_ = 0;
    max_abs_position_ = 0;
    avg_entry_price_dollars_ = 0.0;
    realized_pnl_dollars_ = 0.0;
    total_fees_dollars_ = 0.0;
    gross_traded_notional_dollars_ = 0.0;
    total_traded_shares_ = 0;
    peak_net_mtm_dollars_ = 0.0;
    max_drawdown_dollars_ = 0.0;
}

void Portfolio::on_fill(const FillEvent& fill) {
    if (fill.fill_qty <= 0) {
        return;
    }
    const double fill_px = static_cast<double>(fill.fill_price_ticks) * tick_size_dollars_;
    const double notional = fill_px * static_cast<double>(fill.fill_qty);
    const Qty signed_qty = static_cast<Qty>(side_sign(fill.side)) * fill.fill_qty;

    cash_dollars_ -= static_cast<double>(signed_qty) * fill_px + fill.fee_dollars;
    total_fees_dollars_ += fill.fee_dollars;
    gross_traded_notional_dollars_ += notional;
    total_traded_shares_ += fill.fill_qty;

    if (position_ == 0) {
        position_ = signed_qty;
        avg_entry_price_dollars_ = fill_px;
    } else if ((position_ > 0 && signed_qty > 0) || (position_ < 0 && signed_qty < 0)) {
        // Increasing existing position -> weighted average entry price.
        const double old_abs = static_cast<double>(std::abs(position_));
        const double add_abs = static_cast<double>(fill.fill_qty);
        avg_entry_price_dollars_ =
            (avg_entry_price_dollars_ * old_abs + fill_px * add_abs) / (old_abs + add_abs);
        position_ += signed_qty;
    } else {
        // Reducing or flipping existing position -> realize P&L on closed portion.
        const Qty old_abs = std::abs(position_);
        const Qty closed = std::min(old_abs, fill.fill_qty);
        const double pos_sign = (position_ > 0) ? 1.0 : -1.0;
        realized_pnl_dollars_ +=
            static_cast<double>(closed) * (fill_px - avg_entry_price_dollars_) * pos_sign;
        position_ += signed_qty;
        if (position_ == 0) {
            avg_entry_price_dollars_ = 0.0;
        } else if (fill.fill_qty > old_abs) {
            // Flipped across zero -> remaining shares open at current fill price.
            avg_entry_price_dollars_ = fill_px;
        }
    }

    max_abs_position_ = std::max(max_abs_position_, std::abs(position_));
}

void Portfolio::on_cancel_fee(double fee_dollars) {
    if (fee_dollars == 0.0) {
        return;
    }
    cash_dollars_ -= fee_dollars;
    total_fees_dollars_ += fee_dollars;
}

double Portfolio::unrealized_pnl_dollars(double mark_price_dollars) const noexcept {
    if (position_ == 0) {
        return 0.0;
    }
    return static_cast<double>(position_) * (mark_price_dollars - avg_entry_price_dollars_);
}

double Portfolio::gross_mtm_pnl_dollars(double mark_price_dollars) const noexcept {
    return realized_pnl_dollars_ + unrealized_pnl_dollars(mark_price_dollars);
}

double Portfolio::net_mtm_pnl_dollars(double mark_price_dollars) const noexcept {
    return gross_mtm_pnl_dollars(mark_price_dollars) - total_fees_dollars_;
}

double Portfolio::net_equity_dollars(double mark_price_dollars) const noexcept {
    return cash_dollars_ + static_cast<double>(position_) * mark_price_dollars;
}

double Portfolio::reconciliation_error_dollars(double mark_price_dollars) const noexcept {
    const double lhs = net_equity_dollars(mark_price_dollars);
    const double rhs = initial_cash_dollars_ + net_mtm_pnl_dollars(mark_price_dollars);
    return lhs - rhs;
}

void Portfolio::update_drawdown(double mark_price_dollars) {
    const double net = net_mtm_pnl_dollars(mark_price_dollars);
    if (net > peak_net_mtm_dollars_) {
        peak_net_mtm_dollars_ = net;
    }
    const double dd = peak_net_mtm_dollars_ - net;
    if (dd > max_drawdown_dollars_) {
        max_drawdown_dollars_ = dd;
    }
}

std::vector<HorizonMarkoutStats> compute_markouts(
    const std::vector<FillEvent>& fills, const std::vector<MidSample>& mid_timeline,
    TimestampNs last_market_ts_ns, const std::vector<TimestampNs>& horizons_ns,
    std::vector<FillMarkoutRow>* out_rows) {
    std::vector<HorizonMarkoutStats> stats(horizons_ns.size());
    for (size_t h = 0; h < horizons_ns.size(); ++h) {
        stats[h].horizon_ns = horizons_ns[h];
    }

    if (out_rows != nullptr) {
        out_rows->clear();
        out_rows->reserve(fills.size());
    }

    auto lookup_mid_at = [&](TimestampNs target_ns, double& out_mid) -> bool {
        if (mid_timeline.empty() || target_ns > last_market_ts_ns) {
            return false;
        }
        auto it = std::upper_bound(
            mid_timeline.begin(), mid_timeline.end(), target_ns,
            [](TimestampNs t, const MidSample& s) { return t < s.timestamp_ns; });
        if (it == mid_timeline.begin()) {
            return false;
        }
        --it;
        out_mid = it->mid_ticks;
        return true;
    };

    std::vector<double> sum_cap_ticks(horizons_ns.size(), 0.0);
    std::vector<double> sum_adv_ticks(horizons_ns.size(), 0.0);
    std::vector<double> sum_real_ticks(horizons_ns.size(), 0.0);
    std::vector<double> sum_adv_bps(horizons_ns.size(), 0.0);
    std::vector<double> sum_real_bps(horizons_ns.size(), 0.0);

    for (const auto& f : fills) {
        const double sgn = static_cast<double>(side_sign(f.side));
        const double cap_ticks = sgn * (f.mid_at_fill_ticks - static_cast<double>(f.fill_price_ticks));
        const double w = static_cast<double>(f.fill_qty);

        FillMarkoutRow row{};
        row.fill = f;
        row.captured_half_spread_ticks = cap_ticks;

        for (size_t h = 0; h < horizons_ns.size(); ++h) {
            const TimestampNs h_ns = horizons_ns[h];
            // Strictly enforce timestamp support: never extrapolate past last_market_ts_ns.
            if (f.exchange_time_ns + h_ns > last_market_ts_ns) {
                continue;
            }
            double future_mid = 0.0;
            if (!lookup_mid_at(f.exchange_time_ns + h_ns, future_mid)) {
                continue;
            }
            const double adv_ticks = sgn * (f.mid_at_fill_ticks - future_mid);
            const double real_ticks = cap_ticks - adv_ticks; // == sgn * (future_mid - fill_price)
            const double bps_scale = (f.mid_at_fill_ticks > 0.0) ? (10'000.0 / f.mid_at_fill_ticks) : 0.0;

            ++stats[h].valid_fills;
            stats[h].valid_shares += f.fill_qty;
            sum_cap_ticks[h] += w * cap_ticks;
            sum_adv_ticks[h] += w * adv_ticks;
            sum_real_ticks[h] += w * real_ticks;
            sum_adv_bps[h] += w * adv_ticks * bps_scale;
            sum_real_bps[h] += w * real_ticks * bps_scale;

            if (h_ns == 10 * kNsPerMs) {
                row.has_10ms = true;
                row.adv_sel_10ms_ticks = adv_ticks;
                row.realized_10ms_ticks = real_ticks;
            } else if (h_ns == 100 * kNsPerMs) {
                row.has_100ms = true;
                row.adv_sel_100ms_ticks = adv_ticks;
                row.realized_100ms_ticks = real_ticks;
            } else if (h_ns == 1 * kNsPerSec) {
                row.has_1s = true;
                row.adv_sel_1s_ticks = adv_ticks;
                row.realized_1s_ticks = real_ticks;
            }
        }

        if (out_rows != nullptr) {
            out_rows->push_back(row);
        }
    }

    for (size_t h = 0; h < horizons_ns.size(); ++h) {
        if (stats[h].valid_shares > 0) {
            const double denom = static_cast<double>(stats[h].valid_shares);
            stats[h].mean_captured_half_spread_ticks = sum_cap_ticks[h] / denom;
            stats[h].mean_adverse_selection_ticks = sum_adv_ticks[h] / denom;
            stats[h].mean_realized_spread_ticks = sum_real_ticks[h] / denom;
            stats[h].mean_adverse_selection_bps = sum_adv_bps[h] / denom;
            stats[h].mean_realized_spread_bps = sum_real_bps[h] / denom;
        }
    }

    return stats;
}

} // namespace mmsim
