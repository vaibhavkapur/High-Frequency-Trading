#pragma once

#include "mmsim/config.hpp"
#include "mmsim/exchange.hpp"
#include "mmsim/features.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/portfolio.hpp"
#include "mmsim/risk.hpp"
#include "mmsim/strategy.hpp"
#include "mmsim/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mmsim {

struct SimSummary {
    std::string strategy_name;
    std::string fill_model_name;
    std::string book_impl_name;

    uint64_t total_market_events = 0;
    TimestampNs session_start_ns = 0;
    TimestampNs session_end_ns = 0;
    double session_duration_sec = 0.0;

    uint64_t quotes_submitted = 0;
    uint64_t quotes_accepted = 0;
    uint64_t quotes_risk_rejected = 0;
    uint64_t quotes_exch_rejected = 0;
    uint64_t cancels_submitted = 0;
    uint64_t cancels_succeeded = 0;
    uint64_t cancels_too_late = 0;
    uint64_t cancel_races_filled = 0;

    uint64_t fill_count = 0;
    uint64_t maker_fills = 0;
    uint64_t taker_fills = 0;
    Qty total_quoted_shares = 0;
    Qty total_filled_shares = 0;
    double order_fill_rate = 0.0; // fills / quotes_accepted
    double share_fill_rate = 0.0; // total_filled_shares / total_quoted_shares

    Qty final_position = 0;
    Qty max_abs_position = 0;
    double mean_abs_position = 0.0;
    double rms_position = 0.0;

    double final_mark_price_dollars = 0.0;
    double cash_dollars = 0.0;
    double realized_pnl_dollars = 0.0;
    double unrealized_pnl_dollars = 0.0;
    double gross_mtm_pnl_dollars = 0.0;
    double total_fees_dollars = 0.0;
    double net_mtm_pnl_dollars = 0.0;
    double max_drawdown_dollars = 0.0;
    double reconciliation_error_dollars = 0.0;
    double gross_traded_notional_dollars = 0.0;

    double mean_captured_half_spread_ticks = 0.0;
    double mean_captured_half_spread_bps = 0.0;
    bool drawdown_kill_triggered = false;

    std::vector<HorizonMarkoutStats> markouts;
    RiskStats risk_stats{};
    ExchangeStats exchange_stats{};
};

struct SimResult {
    SimSummary summary{};
    std::vector<FillMarkoutRow> fills;
    std::vector<EquityPoint> equity_curve;
};

class SimulatorEngine {
public:
    explicit SimulatorEngine(const SimConfig& cfg);

    // Runs an entire replay strictly causally and returns complete results.
    SimResult run(const std::vector<MarketEvent>& events, bool record_timeseries = true);

private:
    SimConfig cfg_{};
};

// Output writers for Python analytics & reproducible research.
bool write_summary_json(const std::string& path, const SimSummary& summary, const SimConfig& cfg,
                        std::string& err);
bool write_fills_csv(const std::string& path, const std::vector<FillMarkoutRow>& fills,
                     double tick_size_dollars, std::string& err);
bool write_equity_csv(const std::string& path, const std::vector<EquityPoint>& equity,
                      std::string& err);

} // namespace mmsim
