#include "mmsim/synth.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>

namespace mmsim {

std::string regime_to_string(MarketRegime r) {
    switch (r) {
    case MarketRegime::MeanReverting: return "mean_reverting";
    case MarketRegime::ToxicTrend: return "toxic_trend";
    case MarketRegime::HighVolBurst: return "high_vol_burst";
    case MarketRegime::ThinBook: return "thin_book";
    }
    return "unknown";
}

bool parse_regime(const std::string& s, MarketRegime& out) {
    if (s == "mean_reverting") {
        out = MarketRegime::MeanReverting;
        return true;
    }
    if (s == "toxic_trend") {
        out = MarketRegime::ToxicTrend;
        return true;
    }
    if (s == "high_vol_burst") {
        out = MarketRegime::HighVolBurst;
        return true;
    }
    if (s == "thin_book") {
        out = MarketRegime::ThinBook;
        return true;
    }
    return false;
}

std::vector<MarketEvent> generate_synthetic_session(const SynthParams& base_params) {
    SynthParams p = base_params;
    // Apply regime-specific microstructure calibrations on top of defaults.
    double spread_half_ticks = 1.0;
    double trend_drift_per_sec = 0.0;
    switch (p.regime) {
    case MarketRegime::MeanReverting:
        spread_half_ticks = 1.0;
        break;
    case MarketRegime::ToxicTrend:
        spread_half_ticks = 1.0;
        p.informed_flow_prob = std::max(p.informed_flow_prob, 0.42);
        p.informed_jump_ticks = std::max(p.informed_jump_ticks, 2.2);
        p.mean_reversion_kappa = 0.015;
        trend_drift_per_sec = ((p.seed % 2 == 0) ? 1.0 : -1.0) * 0.45;
        break;
    case MarketRegime::HighVolBurst:
        spread_half_ticks = 1.5;
        p.vol_ticks_per_sqrt_sec = std::max(p.vol_ticks_per_sqrt_sec, 2.4);
        p.informed_flow_prob = std::max(p.informed_flow_prob, 0.30);
        p.informed_jump_ticks = std::max(p.informed_jump_ticks, 1.8);
        break;
    case MarketRegime::ThinBook:
        spread_half_ticks = 1.5;
        p.typical_level_qty = std::max<Qty>(80, p.typical_level_qty / 3);
        p.vol_ticks_per_sqrt_sec = std::max(p.vol_ticks_per_sqrt_sec, 1.6);
        break;
    }

    std::mt19937_64 rng(p.seed);
    std::exponential_distribution<double> dt_dist(p.base_event_rate_hz);
    std::normal_distribution<double> norm(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    std::vector<MarketEvent> events;
    const auto est_events = static_cast<size_t>(p.duration_sec * p.base_event_rate_hz * 1.6) + 256;
    events.reserve(est_events);

    std::map<PriceTicks, Qty> bids;
    std::map<PriceTicks, Qty> asks;

    SeqNo seq = 1;
    auto emit_delta = [&](TimestampNs ts, Side side, PriceTicks price, Qty new_qty) {
        new_qty = std::max<Qty>(0, new_qty);
        auto& book = (side == Side::Bid) ? bids : asks;
        const Qty prev = book.count(price) ? book[price] : 0;
        if (prev == new_qty) {
            return;
        }
        if (new_qty == 0) {
            book.erase(price);
        } else {
            book[price] = new_qty;
        }
        events.push_back(MarketEvent{ts, seq++, MarketEventType::BookDelta, side, price, new_qty});
    };

    auto emit_trade = [&](TimestampNs ts, Side aggressor, PriceTicks price, Qty qty) {
        if (qty <= 0) {
            return;
        }
        events.push_back(MarketEvent{ts, seq++, MarketEventType::Trade, aggressor, price, qty});
    };

    double fair = static_cast<double>(p.initial_mid_ticks);
    double pending_alpha = 0.0; // Unreleased informed drift decaying into fair price over ~150ms
    const TimestampNs end_ns = static_cast<TimestampNs>(p.duration_sec * static_cast<double>(kNsPerSec));

    auto sample_level_qty = [&](int level_idx, double bias_mult) -> Qty {
        const double depth_mult = 1.0 + 0.35 * static_cast<double>(level_idx);
        const double noise = 0.55 + 0.90 * uni(rng);
        const auto q = static_cast<Qty>(
            std::llround(static_cast<double>(p.typical_level_qty) * depth_mult * noise * bias_mult));
        return std::max<Qty>(20, (q / 10) * 10);
    };

    auto sync_book_to_fair = [&](TimestampNs ts, double vol_mult, double imb_tilt) {
        const double eff_half = std::max(0.5, spread_half_ticks * (0.85 + 0.35 * vol_mult));
        PriceTicks target_bid = static_cast<PriceTicks>(std::floor(fair - eff_half));
        PriceTicks target_ask = static_cast<PriceTicks>(std::ceil(fair + eff_half));
        if (target_bid >= target_ask) {
            target_ask = target_bid + 1;
        }
        const PriceTicks min_bid = target_bid - (p.book_depth_levels - 1);
        const PriceTicks max_ask = target_ask + (p.book_depth_levels - 1);

        // Remove stale or crossed levels.
        std::vector<PriceTicks> drop_bids;
        for (const auto& [px, q] : bids) {
            if (px > target_bid || px < min_bid) {
                drop_bids.push_back(px);
            }
        }
        for (PriceTicks px : drop_bids) {
            emit_delta(ts, Side::Bid, px, 0);
        }

        std::vector<PriceTicks> drop_asks;
        for (const auto& [px, q] : asks) {
            if (px < target_ask || px > max_ask) {
                drop_asks.push_back(px);
            }
        }
        for (PriceTicks px : drop_asks) {
            emit_delta(ts, Side::Ask, px, 0);
        }

        // Populate missing levels around target BBO.
        const double bid_mult = std::clamp(1.0 + imb_tilt, 0.35, 1.85);
        const double ask_mult = std::clamp(1.0 - imb_tilt, 0.35, 1.85);
        for (int lvl = 0; lvl < p.book_depth_levels; ++lvl) {
            const PriceTicks bp = target_bid - lvl;
            if (!bids.count(bp)) {
                emit_delta(ts, Side::Bid, bp, sample_level_qty(lvl, bid_mult));
            }
            const PriceTicks ap = target_ask + lvl;
            if (!asks.count(ap)) {
                emit_delta(ts, Side::Ask, ap, sample_level_qty(lvl, ask_mult));
            }
        }
    };

    // Initial book snapshot at t = 0.
    sync_book_to_fair(0, 1.0, 0.0);

    TimestampNs ts = 0;
    while (ts < end_ns) {
        const double dt_sec = std::max(1e-5, dt_dist(rng));
        const auto dt_ns = std::max<TimestampNs>(10'000, static_cast<TimestampNs>(dt_sec * 1e9));
        ts += dt_ns;
        if (ts > end_ns) {
            break;
        }

        // Periodic volatility burst modulation in HighVolBurst regime.
        const double t_sec = static_cast<double>(ts) * 1e-9;
        double vol_mult = 1.0;
        if (p.regime == MarketRegime::HighVolBurst) {
            const double phase = std::fmod(t_sec, 8.0);
            if (phase >= 3.0 && phase <= 5.0) {
                vol_mult = 2.5;
            }
        }

        // Evolve latent fair price: diffusion + mean reversion + regime drift + pending informed alpha.
        const double alpha_release = pending_alpha * (1.0 - std::exp(-dt_sec / 0.14));
        pending_alpha -= alpha_release;
        const double mr = -p.mean_reversion_kappa * (fair - static_cast<double>(p.initial_mid_ticks)) * dt_sec;
        const double diff = p.vol_ticks_per_sqrt_sec * vol_mult * std::sqrt(dt_sec) * norm(rng);
        fair += diff + mr + trend_drift_per_sec * dt_sec + alpha_release;
        fair = std::max(fair, 100.0); // Keep prices strictly positive

        // Mild order-book tilt reflecting upcoming pending_alpha (creates causal microprice/OBI signal).
        const double imb_tilt = std::clamp(0.28 * pending_alpha, -0.55, 0.55);

        const double u = uni(rng);
        if (u < 0.24) {
            // Market trade event (informed or uninformed).
            if (bids.empty() || asks.empty()) {
                sync_book_to_fair(ts, vol_mult, imb_tilt);
                continue;
            }
            const PriceTicks best_bid = bids.rbegin()->first;
            const PriceTicks best_ask = asks.begin()->first;
            const bool informed = (uni(rng) < p.informed_flow_prob);

            Side aggressor = Side::Bid;
            if (informed) {
                // Informed buyer if positive drift/trend or random informed shock; schedules post-trade drift.
                const double buy_prob =
                    (trend_drift_per_sec > 0.0) ? 0.65 : ((trend_drift_per_sec < 0.0) ? 0.35 : 0.50);
                aggressor = (uni(rng) < buy_prob) ? Side::Bid : Side::Ask;
                const double dir = (aggressor == Side::Bid) ? 1.0 : -1.0;
                const double shock = p.informed_jump_ticks * (0.6 + 0.8 * uni(rng)) * vol_mult;
                pending_alpha += dir * shock;
            } else {
                // Uninformed trade leans slightly toward pulling mid back to fair.
                const double book_mid = 0.5 * (static_cast<double>(best_bid) + static_cast<double>(best_ask));
                const double buy_prob = std::clamp(0.5 + 0.12 * (fair - book_mid), 0.25, 0.75);
                aggressor = (uni(rng) < buy_prob) ? Side::Bid : Side::Ask;
            }

            const Side passive_side = opposite_side(aggressor);
            const PriceTicks trade_px = (aggressor == Side::Bid) ? best_ask : best_bid;
            const Qty avail = (passive_side == Side::Bid) ? bids[trade_px] : asks[trade_px];
            const double frac = informed ? (0.35 + 0.65 * uni(rng)) : (0.10 + 0.40 * uni(rng));
            Qty trade_qty = std::max<Qty>(10, (static_cast<Qty>(std::llround(static_cast<double>(avail) * frac)) / 10) * 10);
            trade_qty = std::min(trade_qty, avail);

            emit_trade(ts, aggressor, trade_px, trade_qty);
            const Qty rem = avail - trade_qty;
            emit_delta(ts, passive_side, trade_px, rem);

            // If informed trade depleted top level, occasionally sweep one more tick.
            if (informed && rem == 0 && uni(rng) < 0.30) {
                const PriceTicks next_px = (aggressor == Side::Bid) ? (trade_px + 1) : (trade_px - 1);
                auto& passive_book = (passive_side == Side::Bid) ? bids : asks;
                if (passive_book.count(next_px)) {
                    const Qty avail2 = passive_book[next_px];
                    const Qty sweep2 = std::max<Qty>(10, std::min(avail2, avail2 / 2));
                    emit_trade(ts, aggressor, next_px, sweep2);
                    emit_delta(ts, passive_side, next_px, avail2 - sweep2);
                }
            }
            sync_book_to_fair(ts, vol_mult, imb_tilt);
        } else if (u < 0.72) {
            // Passive limit order add or cancel at one of the book levels.
            sync_book_to_fair(ts, vol_mult, imb_tilt);
            if (!bids.empty() && !asks.empty()) {
                const Side side = (uni(rng) < 0.5) ? Side::Bid : Side::Ask;
                const int lvl = static_cast<int>(uni(rng) * static_cast<double>(p.book_depth_levels));
                const PriceTicks px =
                    (side == Side::Bid) ? (bids.rbegin()->first - lvl) : (asks.begin()->first + lvl);
                auto& book = (side == Side::Bid) ? bids : asks;
                const Qty cur = book.count(px) ? book[px] : 0;
                if (cur > 0) {
                    const double tilt = (side == Side::Bid) ? imb_tilt : -imb_tilt;
                    const bool is_add = (uni(rng) < std::clamp(0.50 + 0.25 * tilt, 0.25, 0.75));
                    const Qty delta =
                        std::max<Qty>(10, (static_cast<Qty>(std::llround(
                                              static_cast<double>(p.typical_level_qty) * (0.10 + 0.35 * uni(rng)))) /
                                          10) *
                                              10);
                    Qty next_q = is_add ? (cur + delta) : std::max<Qty>(20, cur - delta);
                    next_q = std::min<Qty>(next_q, p.typical_level_qty * 6);
                    emit_delta(ts, side, px, next_q);
                }
            }
        } else {
            // Quote shift / book resync around evolving latent fair value.
            sync_book_to_fair(ts, vol_mult, imb_tilt);
        }
    }

    return events;
}

} // namespace mmsim
