#include "mmsim/features.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mmsim {

namespace {
inline constexpr double kLn2 = 0.69314718055994530942;
}

FeatureEngine::FeatureEngine(const FeatureConfig& cfg) : cfg_(cfg) {
    reset();
}

void FeatureEngine::reset() {
    latest_ = BookSignals{};
    latest_.realized_vol_ticks_per_sqrt_sec = cfg_.min_vol_ticks_per_sqrt_sec;
    has_prev_tob_ = false;
    prev_tob_ = TopOfBook{};
    prev_event_ns_ = 0;
    prev_mid_ns_ = 0;
    ewma_ofi_ = 0.0;
    ewma_ofi_abs_ = 100.0;
    ewma_var_per_sec_ = cfg_.min_vol_ticks_per_sqrt_sec * cfg_.min_vol_ticks_per_sqrt_sec;
}

BookSignals FeatureEngine::on_event(const MarketEvent& ev, const IL2Book& book) {
    const TimestampNs now_ns = ev.timestamp_ns;
    const TopOfBook tob = book.top_of_book();

    double dt_sec = 0.0;
    if (has_prev_tob_ && now_ns > prev_event_ns_) {
        dt_sec = static_cast<double>(now_ns - prev_event_ns_) * 1e-9;
    }

    // Exponential decay for short-horizon OFI.
    if (dt_sec > 0.0 && cfg_.ofi_halflife_sec > 0.0) {
        const double decay = std::exp(-dt_sec * kLn2 / cfg_.ofi_halflife_sec);
        ewma_ofi_ *= decay;
        ewma_ofi_abs_ = ewma_ofi_abs_ * decay + 50.0 * (1.0 - decay);
    }

    // Add causal OFI impulse from current event only.
    double ofi_impulse = 0.0;
    if (ev.type == MarketEventType::Trade) {
        ofi_impulse = static_cast<double>(side_sign(ev.side) * ev.qty);
    } else if (ev.type == MarketEventType::BookDelta && has_prev_tob_ && prev_tob_.valid() &&
               tob.valid()) {
        // Cont-Kukanov-Stoikov top-of-book order-flow imbalance.
        double bid_contrib = 0.0;
        if (tob.best_bid > prev_tob_.best_bid) {
            bid_contrib = static_cast<double>(tob.bid_qty);
        } else if (tob.best_bid == prev_tob_.best_bid) {
            bid_contrib = static_cast<double>(tob.bid_qty - prev_tob_.bid_qty);
        } else {
            bid_contrib = -static_cast<double>(prev_tob_.bid_qty);
        }

        double ask_contrib = 0.0;
        if (tob.best_ask < prev_tob_.best_ask) {
            ask_contrib = -static_cast<double>(tob.ask_qty);
        } else if (tob.best_ask == prev_tob_.best_ask) {
            ask_contrib = -static_cast<double>(tob.ask_qty - prev_tob_.ask_qty);
        } else {
            ask_contrib = static_cast<double>(prev_tob_.ask_qty);
        }
        ofi_impulse = 0.5 * (bid_contrib + ask_contrib);
    }

    if (ofi_impulse != 0.0) {
        ewma_ofi_ += ofi_impulse;
        ewma_ofi_abs_ = 0.95 * ewma_ofi_abs_ + 0.05 * std::abs(ofi_impulse);
    }

    // Realized volatility EWMA (ticks^2 / second), using time since last valid ToB update.
    if (has_prev_tob_ && prev_tob_.valid() && tob.valid()) {
        const double prev_mid = prev_tob_.mid_ticks();
        const double cur_mid = tob.mid_ticks();
        const double d_mid = cur_mid - prev_mid;
        const double dt_mid_sec =
            (now_ns > prev_mid_ns_) ? (static_cast<double>(now_ns - prev_mid_ns_) * 1e-9) : 0.0;
        if (dt_mid_sec > 0.0) {
            const double var_decay =
                (cfg_.vol_halflife_sec > 0.0) ? std::exp(-dt_mid_sec * kLn2 / cfg_.vol_halflife_sec) : 0.0;
            const double inst_var = std::min(400.0, (d_mid * d_mid) / std::max(dt_mid_sec, 1e-4));
            ewma_var_per_sec_ = var_decay * ewma_var_per_sec_ + (1.0 - var_decay) * inst_var;
        }
    }

    BookSignals sig{};
    sig.timestamp_ns = now_ns;
    sig.tob = tob;
    sig.valid = tob.valid();

    if (sig.valid) {
        sig.mid_ticks = tob.mid_ticks();
        sig.spread_ticks = tob.spread_ticks();
        sig.spread_bps =
            (sig.mid_ticks > 0.0)
                ? (static_cast<double>(sig.spread_ticks) / sig.mid_ticks * 10'000.0)
                : 0.0;

        const double bq = static_cast<double>(tob.bid_qty);
        const double aq = static_cast<double>(tob.ask_qty);
        const double sum_l1 = bq + aq;
        if (sum_l1 > 0.0) {
            sig.microprice_ticks =
                (static_cast<double>(tob.best_bid) * aq + static_cast<double>(tob.best_ask) * bq) /
                sum_l1;
            sig.l1_imbalance = (bq - aq) / sum_l1;
        } else {
            sig.microprice_ticks = sig.mid_ticks;
            sig.l1_imbalance = 0.0;
        }

        // Depth-weighted top-K order-book imbalance (zero allocations).
        std::array<PriceLevel, kMaxFeatureDepth> bid_lvls{};
        std::array<PriceLevel, kMaxFeatureDepth> ask_lvls{};
        const size_t k = static_cast<size_t>(std::clamp(cfg_.imbalance_depth, 1,
                                                        static_cast<int>(kMaxFeatureDepth)));
        const size_t nb = book.top_levels(Side::Bid, k, bid_lvls);
        const size_t na = book.top_levels(Side::Ask, k, ask_lvls);
        double wb = 0.0;
        double wa = 0.0;
        for (size_t i = 0; i < nb; ++i) {
            wb += static_cast<double>(bid_lvls[i].qty) / static_cast<double>(i + 1);
        }
        for (size_t i = 0; i < na; ++i) {
            wa += static_cast<double>(ask_lvls[i].qty) / static_cast<double>(i + 1);
        }
        sig.topk_imbalance = (wb + wa > 0.0) ? ((wb - wa) / (wb + wa)) : 0.0;
        sig.short_horizon_ofi = std::tanh(ewma_ofi_ / std::max(50.0, ewma_ofi_abs_ * 4.0));
        sig.realized_vol_ticks_per_sqrt_sec =
            std::max(cfg_.min_vol_ticks_per_sqrt_sec, std::sqrt(std::max(0.0, ewma_var_per_sec_)));
    } else {
        sig.realized_vol_ticks_per_sqrt_sec = cfg_.min_vol_ticks_per_sqrt_sec;
    }

    if (tob.valid()) {
        prev_tob_ = tob;
        prev_mid_ns_ = now_ns;
        has_prev_tob_ = true;
    }
    prev_event_ns_ = now_ns;
    latest_ = sig;
    return sig;
}

} // namespace mmsim
