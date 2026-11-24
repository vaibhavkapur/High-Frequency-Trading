#pragma once

#include "mmsim/config.hpp"
#include "mmsim/order_book.hpp"
#include "mmsim/types.hpp"

namespace mmsim {

struct BookSignals {
    TimestampNs timestamp_ns = 0;
    bool valid = false;

    TopOfBook tob{};
    double mid_ticks = 0.0;
    double microprice_ticks = 0.0;
    PriceTicks spread_ticks = 0;
    double spread_bps = 0.0;

    double l1_imbalance = 0.0;              // [-1, +1]
    double topk_imbalance = 0.0;            // Depth-weighted top-K imbalance in [-1, +1]
    double short_horizon_ofi = 0.0;         // Normalized EWMA order-flow imbalance in [-1, +1]
    double realized_vol_ticks_per_sqrt_sec = 0.25;
};

// Strictly causal online book & order-flow feature extractor.
class FeatureEngine {
public:
    explicit FeatureEngine(const FeatureConfig& cfg);

    void reset();

    // Must be called strictly after updating `book` with `ev`.
    BookSignals on_event(const MarketEvent& ev, const IL2Book& book);

    [[nodiscard]] const BookSignals& latest() const noexcept { return latest_; }

private:
    FeatureConfig cfg_{};
    BookSignals latest_{};

    bool has_prev_tob_ = false;
    TopOfBook prev_tob_{};
    TimestampNs prev_event_ns_ = 0;
    TimestampNs prev_mid_ns_ = 0;

    double ewma_ofi_ = 0.0;
    double ewma_ofi_abs_ = 100.0;
    double ewma_var_per_sec_ = 0.25;
};

} // namespace mmsim
