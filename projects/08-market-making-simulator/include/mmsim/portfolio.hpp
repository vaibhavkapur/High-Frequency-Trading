#pragma once

#include "mmsim/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mmsim {

struct EquityPoint {
    TimestampNs timestamp_ns = 0;
    double mid_ticks = 0.0;
    Qty position = 0;
    double cash_dollars = 0.0;
    double realized_pnl_dollars = 0.0;
    double unrealized_pnl_dollars = 0.0;
    double total_fees_dollars = 0.0;
    double gross_mtm_pnl_dollars = 0.0;
    double net_mtm_pnl_dollars = 0.0;
};

struct MidSample {
    TimestampNs timestamp_ns = 0;
    double mid_ticks = 0.0;
};

struct HorizonMarkoutStats {
    TimestampNs horizon_ns = 0;
    uint64_t valid_fills = 0;
    Qty valid_shares = 0;
    double mean_captured_half_spread_ticks = 0.0;
    double mean_adverse_selection_ticks = 0.0;
    double mean_realized_spread_ticks = 0.0;
    double mean_adverse_selection_bps = 0.0;
    double mean_realized_spread_bps = 0.0;
};

struct FillMarkoutRow {
    FillEvent fill{};
    double captured_half_spread_ticks = 0.0;
    bool has_10ms = false;
    double adv_sel_10ms_ticks = 0.0;
    double realized_10ms_ticks = 0.0;
    bool has_100ms = false;
    double adv_sel_100ms_ticks = 0.0;
    double realized_100ms_ticks = 0.0;
    bool has_1s = false;
    double adv_sel_1s_ticks = 0.0;
    double realized_1s_ticks = 0.0;
};

class Portfolio {
public:
    explicit Portfolio(double tick_size_dollars = 0.01, double initial_cash_dollars = 0.0);

    void reset(double tick_size_dollars, double initial_cash_dollars);

    void on_fill(const FillEvent& fill);
    void on_cancel_fee(double fee_dollars);

    [[nodiscard]] Qty position() const noexcept { return position_; }
    [[nodiscard]] Qty max_abs_position_observed() const noexcept { return max_abs_position_; }
    [[nodiscard]] double initial_cash_dollars() const noexcept { return initial_cash_dollars_; }
    [[nodiscard]] double cash_dollars() const noexcept { return cash_dollars_; }
    [[nodiscard]] double avg_entry_price_dollars() const noexcept { return avg_entry_price_dollars_; }
    [[nodiscard]] double realized_pnl_dollars() const noexcept { return realized_pnl_dollars_; }
    [[nodiscard]] double total_fees_dollars() const noexcept { return total_fees_dollars_; }
    [[nodiscard]] double gross_traded_notional_dollars() const noexcept {
        return gross_traded_notional_dollars_;
    }
    [[nodiscard]] Qty total_traded_shares() const noexcept { return total_traded_shares_; }

    [[nodiscard]] double unrealized_pnl_dollars(double mark_price_dollars) const noexcept;
    [[nodiscard]] double gross_mtm_pnl_dollars(double mark_price_dollars) const noexcept;
    [[nodiscard]] double net_mtm_pnl_dollars(double mark_price_dollars) const noexcept;
    [[nodiscard]] double net_equity_dollars(double mark_price_dollars) const noexcept;

    // Exact accounting identity error: (cash + pos*mark) - (initial_cash + net_mtm_pnl).
    [[nodiscard]] double reconciliation_error_dollars(double mark_price_dollars) const noexcept;

    void update_drawdown(double mark_price_dollars);
    [[nodiscard]] double peak_net_mtm_dollars() const noexcept { return peak_net_mtm_dollars_; }
    [[nodiscard]] double max_drawdown_dollars() const noexcept { return max_drawdown_dollars_; }

private:
    double tick_size_dollars_ = 0.01;
    double initial_cash_dollars_ = 0.0;
    double cash_dollars_ = 0.0;
    Qty position_ = 0;
    Qty max_abs_position_ = 0;
    double avg_entry_price_dollars_ = 0.0;
    double realized_pnl_dollars_ = 0.0;
    double total_fees_dollars_ = 0.0;
    double gross_traded_notional_dollars_ = 0.0;
    Qty total_traded_shares_ = 0;
    double peak_net_mtm_dollars_ = 0.0;
    double max_drawdown_dollars_ = 0.0;
};

// Computes markouts ONLY where fill.exchange_time_ns + horizon_ns <= last_market_ts_ns.
std::vector<HorizonMarkoutStats> compute_markouts(
    const std::vector<FillEvent>& fills, const std::vector<MidSample>& mid_timeline,
    TimestampNs last_market_ts_ns, const std::vector<TimestampNs>& horizons_ns,
    std::vector<FillMarkoutRow>* out_rows = nullptr);

} // namespace mmsim
