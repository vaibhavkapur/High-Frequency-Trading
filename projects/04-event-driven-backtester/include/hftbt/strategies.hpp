// Baseline strategies. They are reference users of the API and workloads for
// the benchmarks -- not trading recommendations.
#pragma once

#include <vector>

#include "hftbt/strategy.hpp"

namespace hftbt {

class NoopStrategy final : public Strategy {};

// Two-sided quoting around the observed mid with linear inventory skew.
struct MarketMakerParams {
    Price half_spread_ticks = 2;
    Qty order_qty = 10;
    Qty max_position = 50;
    double skew_ticks_per_lot = 0.05; // reservation = mid - skew * position
    Price requote_threshold_ticks = 1;
    Timestamp min_requote_interval_ns = 0;
    bool post_only = true;
};

class MarketMaker final : public Strategy {
public:
    explicit MarketMaker(const MarketMakerParams& p) : p_(p) {}
    void on_start(Context& ctx) override;
    void on_market_data(Context& ctx, const MarketEvent& ev) override;
    void on_order_update(Context& ctx, const ExecReport& r) override;

private:
    struct Quote {
        OrderId id = 0;
        Price px = 0;
        bool cancelling = false;
        Timestamp last_action = -(Timestamp{1} << 60);
    };
    struct State {
        Quote q[2];
    };
    void requote(Context& ctx, InstrumentId i);
    void manage(Context& ctx, InstrumentId i, Side side, bool want, Price px, Qty qty);

    MarketMakerParams p_;
    std::vector<State> st_;
};

// Takes liquidity when top-of-book size imbalance is extreme.
struct ImbalanceParams {
    double threshold = 0.6; // |(bid_qty - ask_qty) / (bid_qty + ask_qty)|
    Qty order_qty = 5;
    Qty max_position = 20;
    Timestamp cooldown_ns = 1'000'000;
    Timestamp max_hold_ns = 0; // > 0: flatten positions older than this
};

class ImbalanceTaker final : public Strategy {
public:
    explicit ImbalanceTaker(const ImbalanceParams& p) : p_(p) {}
    void on_start(Context& ctx) override;
    void on_market_data(Context& ctx, const MarketEvent& ev) override;
    void on_order_update(Context& ctx, const ExecReport& r) override;

private:
    struct State {
        OrderId pending = 0;
        Timestamp last_send = -(Timestamp{1} << 60);
        Timestamp entry_ts = 0;
    };
    ImbalanceParams p_;
    std::vector<State> st_;
};

// Rests `levels` passive orders per side away from the touch and leaves them
// there. Exists to measure how runtime scales with active simulated orders.
struct LadderParams {
    std::uint32_t levels = 5;
    Qty order_qty = 1;
    Price offset_ticks = 3;
};

class Ladder final : public Strategy {
public:
    explicit Ladder(const LadderParams& p) : p_(p) {}
    void on_start(Context& ctx) override;
    void on_market_data(Context& ctx, const MarketEvent& ev) override;

private:
    LadderParams p_;
    std::vector<bool> placed_;
};

} // namespace hftbt
