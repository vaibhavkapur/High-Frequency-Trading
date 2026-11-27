#pragma once

#include "mmae/order_book.hpp"
#include "mmae/types.hpp"

#include <deque>
#include <memory>
#include <string>

namespace mmae {

struct ProcessResult {
    bool accepted = false;
    ValidationErrorCode error = ValidationErrorCode::NONE;
    std::string error_detail;
    FeatureRow feature;
};

// Strictly causal single-pass event-time microstructure feature engine.
// Invariant: FeatureRow emitted at event i depends ONLY on events 0..i.
class FeatureEngine {
public:
    explicit FeatureEngine(const EngineConfig& config);

    ProcessResult process_event(const Event& event, size_t line_number = 0);

    const Diagnostics& diagnostics() const { return diagnostics_; }
    const EngineConfig& config() const { return config_; }
    const IOrderBook& book() const { return *book_; }

private:
    struct TimedOfi {
        int64_t ts_ns = 0;
        double ofi = 0.0;
    };

    struct TimedTradeFlow {
        int64_t ts_ns = 0;
        int64_t signed_qty = 0;
        int64_t total_qty = 0;
        int64_t ambiguous_qty = 0;
    };

    void record_error(size_t line_number, uint64_t seq_num, ValidationErrorCode code, const std::string& detail);
    void prune_windows(int64_t now_ts_ns);
    double compute_event_ofi(const TopOfBook& prev, const TopOfBook& curr) const;

    EngineConfig config_;
    std::unique_ptr<IOrderBook> book_;
    Diagnostics diagnostics_;

    bool has_seen_event_ = false;
    std::string stream_symbol_;
    int64_t last_event_ts_ns_ = 0;
    uint64_t last_seq_num_ = 0;

    bool has_quote_ts_ = false;
    int64_t last_quote_ts_ns_ = 0;

    bool gap_active_ = false;
    bool has_prev_valid_tob_ = false;
    TopOfBook prev_valid_tob_{};

    // Tick-rule diagnostic state (strictly causal prior trades only).
    bool has_prev_trade_ = false;
    int64_t prev_trade_price_ticks_ = 0;
    int8_t last_nonzero_tick_dir_ = 0;

    // Rolling causal windows bounded by flow_window_ns.
    std::deque<TimedOfi> ofi_window_;
    double rolling_ofi_sum_ = 0.0;

    std::deque<TimedTradeFlow> trade_window_;
    int64_t rolling_signed_flow_qty_ = 0;
    int64_t rolling_total_trade_qty_ = 0;
    int64_t rolling_ambiguous_qty_ = 0;

    // Cumulative L3 counts for causal cancel-to-add ratio.
    uint64_t cumulative_adds_ = 0;
    uint64_t cumulative_cancels_ = 0;
};

}  // namespace mmae
