#include "mmsim/strategy.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

// ============================================================================
// NaiveSymmetricStrategy
// ============================================================================

NaiveSymmetricStrategy::NaiveSymmetricStrategy(const StrategyConfig& cfg) : cfg_(cfg) {}

QuoteDecision NaiveSymmetricStrategy::compute_quotes(const BookSignals& signals,
                                                     const StrategyState& /*state*/) const {
    QuoteDecision out{};
    if (!signals.valid) {
        return out;
    }

    const double half = std::max(cfg_.min_half_spread_ticks, cfg_.base_half_spread_ticks);
    out.fair_value_ticks = signals.mid_ticks;
    out.reservation_price_ticks = signals.mid_ticks;
    out.half_spread_ticks = half;

    PriceTicks bid_px = static_cast<PriceTicks>(std::floor(signals.mid_ticks - half));
    PriceTicks ask_px = static_cast<PriceTicks>(std::ceil(signals.mid_ticks + half));

    if (cfg_.post_only) {
        bid_px = std::min(bid_px, signals.tob.best_ask - 1);
        ask_px = std::max(ask_px, signals.tob.best_bid + 1);
    }
    if (bid_px >= ask_px) {
        ask_px = bid_px + 1;
    }

    if (bid_px > 0 && cfg_.base_quote_qty > 0) {
        out.has_bid = true;
        out.bid_price_ticks = bid_px;
        out.bid_qty = cfg_.base_quote_qty;
    }
    if (ask_px > 0 && cfg_.base_quote_qty > 0) {
        out.has_ask = true;
        out.ask_price_ticks = ask_px;
        out.ask_qty = cfg_.base_quote_qty;
    }
    return out;
}

// ============================================================================
// InventoryAwareStrategy
// ============================================================================

InventoryAwareStrategy::InventoryAwareStrategy(const StrategyConfig& cfg) : cfg_(cfg) {}

QuoteDecision InventoryAwareStrategy::compute_quotes(const BookSignals& signals,
                                                     const StrategyState& state) const {
    QuoteDecision out{};
    if (!signals.valid) {
        return out;
    }

    // 1. Causal fair-value estimate combining microprice, multi-level imbalance, and short OFI.
    const double market_half_spread = 0.5 * static_cast<double>(signals.spread_ticks);
    const double fair_value = signals.microprice_ticks +
                              cfg_.alpha_obi * signals.topk_imbalance * market_half_spread +
                              cfg_.alpha_ofi * signals.short_horizon_ofi;

    // 2. Inventory-aware reservation price (linear + quadratic urgency near position limits).
    const double max_pos = static_cast<double>(std::max<Qty>(1, state.max_abs_position));
    const double q_norm = std::clamp(static_cast<double>(state.position) / max_pos, -1.0, 1.0);
    const double vol = std::max(0.25, signals.realized_vol_ticks_per_sqrt_sec);
    const double skew_factor = q_norm + cfg_.inventory_quad_skew * q_norm * std::abs(q_norm);
    const double inventory_penalty = cfg_.risk_aversion * skew_factor * std::max(1.0, vol);
    const double reservation = fair_value - inventory_penalty;

    // 3. Volatility- and fee-adjusted half-spread.
    const double fee_ticks =
        (state.maker_fee_bps > 0.0)
            ? (state.maker_fee_bps * 1e-4 * signals.mid_ticks * cfg_.fee_spread_mult)
            : 0.0;
    const double raw_half = cfg_.base_half_spread_ticks + cfg_.vol_spread_mult * vol + fee_ticks;
    const double half_spread =
        std::clamp(raw_half, cfg_.min_half_spread_ticks, cfg_.max_half_spread_ticks);

    out.fair_value_ticks = fair_value;
    out.reservation_price_ticks = reservation;
    out.half_spread_ticks = half_spread;

    PriceTicks bid_px = static_cast<PriceTicks>(std::floor(reservation - half_spread));
    PriceTicks ask_px = static_cast<PriceTicks>(std::ceil(reservation + half_spread));

    if (cfg_.post_only) {
        bid_px = std::min(bid_px, signals.tob.best_ask - 1);
        ask_px = std::max(ask_px, signals.tob.best_bid + 1);
    }
    if (bid_px >= ask_px) {
        if (q_norm >= 0.0) {
            bid_px = ask_px - 1;
        } else {
            ask_px = bid_px + 1;
        }
    }

    // 4. Inventory-aware size throttling (reduce inventory-increasing side as position grows).
    auto round_lot = [](Qty q) -> Qty { return std::max<Qty>(10, (q / 10) * 10); };

    Qty bid_qty = cfg_.base_quote_qty;
    Qty ask_qty = cfg_.base_quote_qty;
    bool quote_bid = (bid_px > 0);
    bool quote_ask = (ask_px > 0);

    if (q_norm > 0.0) {
        if (q_norm >= 0.90) {
            quote_bid = false;
        } else {
            const double scale = std::clamp(1.0 - q_norm, 0.0, 1.0);
            const Qty scaled =
                static_cast<Qty>(std::llround(static_cast<double>(cfg_.base_quote_qty) * scale));
            bid_qty = std::max(cfg_.min_quote_qty, round_lot(scaled));
        }
    } else if (q_norm < 0.0) {
        const double abs_q = -q_norm;
        if (abs_q >= 0.90) {
            quote_ask = false;
        } else {
            const double scale = std::clamp(1.0 - abs_q, 0.0, 1.0);
            const Qty scaled =
                static_cast<Qty>(std::llround(static_cast<double>(cfg_.base_quote_qty) * scale));
            ask_qty = std::max(cfg_.min_quote_qty, round_lot(scaled));
        }
    }

    if (quote_bid && bid_qty > 0) {
        out.has_bid = true;
        out.bid_price_ticks = bid_px;
        out.bid_qty = bid_qty;
    }
    if (quote_ask && ask_qty > 0) {
        out.has_ask = true;
        out.ask_price_ticks = ask_px;
        out.ask_qty = ask_qty;
    }
    return out;
}

std::unique_ptr<IStrategy> make_strategy(const StrategyConfig& cfg) {
    switch (cfg.type) {
    case StrategyType::InventoryAware: return std::make_unique<InventoryAwareStrategy>(cfg);
    case StrategyType::NaiveSymmetric: return std::make_unique<NaiveSymmetricStrategy>(cfg);
    }
    return std::make_unique<InventoryAwareStrategy>(cfg);
}

} // namespace mmsim
