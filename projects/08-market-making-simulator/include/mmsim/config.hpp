#pragma once

#include "mmsim/types.hpp"

#include <cstdint>
#include <string>

namespace mmsim {

enum class StrategyType : uint8_t {
    InventoryAware = 0,
    NaiveSymmetric = 1,
};

enum class FillModelType : uint8_t {
    QueuePriority = 0,      // Explicit estimated queue-ahead depletion + trade-through
    ProbabilisticTouch = 1, // Distance-decaying probabilistic fill on market trades
};

enum class BookImplType : uint8_t {
    FlatArray = 0, // Cache-friendly fixed-window tick array
    StdMap = 1,    // Reference std::map book (for differential & perf comparison)
};

struct LatencyConfig {
    TimestampNs md_latency_ns = 100 * kNsPerUs;    // Market data arrival delay (100 us)
    TimestampNs order_latency_ns = 150 * kNsPerUs; // Order/cancel transit to exchange (150 us)
    TimestampNs ack_latency_ns = 100 * kNsPerUs;   // Ack/fill transit to strategy (100 us)
};

struct FeeConfig {
    double maker_fee_bps = -0.2; // Negative = rebate (-0.2 bps)
    double taker_fee_bps = 1.5;  // Positive = fee (+1.5 bps)
    double cancel_fee_dollars = 0.0;
};

struct RiskConfig {
    Qty max_abs_position = 500;
    Qty max_order_size = 100;
    double max_gross_notional_dollars = 150000.0;
    double max_drawdown_dollars = 1500.0;
    int max_open_orders = 4;
};

struct FeatureConfig {
    int imbalance_depth = 5;
    double ofi_halflife_sec = 0.5;
    double vol_halflife_sec = 2.0;
    double min_vol_ticks_per_sqrt_sec = 0.25;
};

struct StrategyConfig {
    StrategyType type = StrategyType::InventoryAware;
    double tick_size_dollars = 0.01;

    // Fair value weights (InventoryAware only)
    double alpha_obi = 0.35; // Multiplier on top-K imbalance * half_spread
    double alpha_ofi = 0.15; // Multiplier on normalized short-horizon OFI (in ticks)

    // Quoting & inventory skew parameters
    double base_half_spread_ticks = 1.0;
    double min_half_spread_ticks = 1.0;
    double max_half_spread_ticks = 12.0;
    double vol_spread_mult = 0.40; // Added half-spread per tick/sqrt(s) of vol
    double fee_spread_mult = 1.0;  // Extra half-spread buffer for positive maker fees
    double risk_aversion = 1.2;    // Reservation price skew strength
    double inventory_quad_skew = 0.5; // Extra quadratic skew near max position

    // Order sizing & hysteresis
    Qty base_quote_qty = 100;
    Qty min_quote_qty = 20;
    PriceTicks requote_threshold_ticks = 1;
    TimestampNs min_quote_lifetime_ns = 5 * kNsPerMs;
    bool post_only = true;
};

struct ExchangeConfig {
    FillModelType fill_model = FillModelType::QueuePriority;
    double queue_cancel_ahead_fraction = 0.5; // Fraction of level cancels assumed ahead of us
    double touch_fill_prob = 0.25;            // Used by ProbabilisticTouch at top-of-book
    double distance_decay_per_tick = 0.65;    // Decay factor per tick away from mid
    uint64_t rng_seed = 42;
};

struct SimConfig {
    double tick_size_dollars = 0.01;
    double initial_cash_dollars = 0.0;
    BookImplType book_impl = BookImplType::FlatArray;
    LatencyConfig latency{};
    FeeConfig fees{};
    RiskConfig risk{};
    FeatureConfig features{};
    StrategyConfig strategy{};
    ExchangeConfig exchange{};
    TimestampNs equity_sample_interval_ns = 100 * kNsPerMs; // 100 ms sampling for equity curve
};

// Loads key=value config overrides from a simple config file (# comments supported).
bool load_config_file(const std::string& path, SimConfig& out, std::string& err);

// Applies a single "key=value" override to cfg.
bool apply_config_kv(const std::string& key, const std::string& value, SimConfig& cfg,
                     std::string& err);

std::string strategy_type_to_string(StrategyType t);
std::string fill_model_to_string(FillModelType f);
std::string book_impl_to_string(BookImplType b);

} // namespace mmsim
