#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace sor {

using VenueId = std::uint8_t;
using OrderId = std::uint64_t;
using ExecId = std::uint64_t;
using TimestampNs = std::int64_t;
using Quantity = std::int64_t;

// Fixed-point price ticks: 1 tick = $0.0001 (1e-4 USD / 1 mil).
// Example: $150.2500 -> 1,502,500 ticks; $0.0030 fee -> 30 ticks.
using PriceTicks = std::int64_t;
inline constexpr PriceTicks kPriceScale = 10'000;
inline constexpr PriceTicks kInvalidPrice = std::numeric_limits<PriceTicks>::min();

inline constexpr std::size_t kMaxVenues = 8;
inline constexpr std::size_t kMaxBookDepth = 16;
inline constexpr VenueId kInvalidVenue = 255;

inline constexpr PriceTicks usd_to_ticks(double usd) noexcept {
    return static_cast<PriceTicks>(std::llround(usd * static_cast<double>(kPriceScale)));
}

inline constexpr double ticks_to_usd(PriceTicks ticks) noexcept {
    return static_cast<double>(ticks) / static_cast<double>(kPriceScale);
}

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

inline constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

enum class TimeInForce : std::uint8_t {
    IOC = 0,  // Immediate-or-Cancel (sweep available liquidity, cancel unfilled remainder)
    FOK = 1,  // Fill-or-Kill (route only if total eligible liquidity >= parent qty)
    DAY = 2,  // Limit order that can re-route across waves until filled or expired
};

inline constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::DAY: return "DAY";
    }
    return "UNKNOWN";
}

enum class RoutingPolicy : std::uint8_t {
    NaivePrimary = 0,    // Baseline: always route to primary venue (venue 0)
    NaiveBestPrice = 1,  // Baseline: raw displayed price only (ignores fees, latency, staleness)
    FeeAware = 2,        // Fee-adjusted effective price + staleness/health gating
    SmartCostAware = 3,  // Full expected all-in cost (price + fees + latency drift + miss penalty)
};

inline constexpr std::string_view to_string(RoutingPolicy p) noexcept {
    switch (p) {
        case RoutingPolicy::NaivePrimary: return "naive_primary";
        case RoutingPolicy::NaiveBestPrice: return "naive_best_price";
        case RoutingPolicy::FeeAware: return "fee_aware";
        case RoutingPolicy::SmartCostAware: return "smart_cost_aware";
    }
    return "unknown";
}

enum class VenueHealthState : std::uint8_t {
    Healthy = 0,
    Degraded = 1,
    CircuitOpen = 2,  // Failing / isolated; cannot receive new child orders
};

inline constexpr std::string_view to_string(VenueHealthState s) noexcept {
    switch (s) {
        case VenueHealthState::Healthy: return "HEALTHY";
        case VenueHealthState::Degraded: return "DEGRADED";
        case VenueHealthState::CircuitOpen: return "CIRCUIT_OPEN";
    }
    return "UNKNOWN";
}

enum class IneligibilityReason : std::uint8_t {
    Eligible = 0,
    Disabled = 1,
    VenueDown = 2,
    StaleQuote = 3,
    NoLiquidity = 4,
    PriceLimit = 5,
    ChildOrderCap = 6,
    BelowMinQty = 7,
};

inline constexpr std::string_view to_string(IneligibilityReason r) noexcept {
    switch (r) {
        case IneligibilityReason::Eligible: return "ELIGIBLE";
        case IneligibilityReason::Disabled: return "DISABLED";
        case IneligibilityReason::VenueDown: return "VENUE_DOWN";
        case IneligibilityReason::StaleQuote: return "STALE_QUOTE";
        case IneligibilityReason::NoLiquidity: return "NO_LIQUIDITY";
        case IneligibilityReason::PriceLimit: return "PRICE_LIMIT";
        case IneligibilityReason::ChildOrderCap: return "CHILD_ORDER_CAP";
        case IneligibilityReason::BelowMinQty: return "BELOW_MIN_QTY";
    }
    return "UNKNOWN";
}

enum class ParentOrderState : std::uint8_t {
    New = 0,
    Active = 1,
    PartiallyFilled = 2,
    Filled = 3,
    Cancelled = 4,
    Rejected = 5,
    Expired = 6,
};

inline constexpr std::string_view to_string(ParentOrderState s) noexcept {
    switch (s) {
        case ParentOrderState::New: return "NEW";
        case ParentOrderState::Active: return "ACTIVE";
        case ParentOrderState::PartiallyFilled: return "PARTIALLY_FILLED";
        case ParentOrderState::Filled: return "FILLED";
        case ParentOrderState::Cancelled: return "CANCELLED";
        case ParentOrderState::Rejected: return "REJECTED";
        case ParentOrderState::Expired: return "EXPIRED";
    }
    return "UNKNOWN";
}

inline constexpr bool is_terminal(ParentOrderState s) noexcept {
    return s == ParentOrderState::Filled || s == ParentOrderState::Cancelled ||
           s == ParentOrderState::Rejected || s == ParentOrderState::Expired;
}

enum class ChildOrderState : std::uint8_t {
    PendingNew = 0,
    Acked = 1,
    PartiallyFilled = 2,
    Filled = 3,
    PendingCancel = 4,
    Cancelled = 5,
    PendingReplace = 6,
    Rejected = 7,
};

inline constexpr std::string_view to_string(ChildOrderState s) noexcept {
    switch (s) {
        case ChildOrderState::PendingNew: return "PENDING_NEW";
        case ChildOrderState::Acked: return "ACKED";
        case ChildOrderState::PartiallyFilled: return "PARTIALLY_FILLED";
        case ChildOrderState::Filled: return "FILLED";
        case ChildOrderState::PendingCancel: return "PENDING_CANCEL";
        case ChildOrderState::Cancelled: return "CANCELLED";
        case ChildOrderState::PendingReplace: return "PENDING_REPLACE";
        case ChildOrderState::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

inline constexpr bool is_terminal(ChildOrderState s) noexcept {
    return s == ChildOrderState::Filled || s == ChildOrderState::Cancelled ||
           s == ChildOrderState::Rejected;
}

struct PriceLevel {
    PriceTicks price{kInvalidPrice};
    Quantity quantity{0};
    std::uint32_t order_count{0};
};

// Static + configurable properties of a simulated trading venue.
struct VenueConfig {
    VenueId id{0};
    std::string name{"VENUE"};
    bool enabled{true};

    // Fee model: explicit taker fee can be positive (fee) or negative (rebate).
    // Applied as: per_share_ticks + raw_price * (bps / 10,000).
    PriceTicks taker_fee_per_share_ticks{30};  // Default $0.0030/share
    double taker_fee_bps{0.0};
    PriceTicks maker_fee_per_share_ticks{-20};  // Default -$0.0020/share rebate
    double maker_fee_bps{0.0};

    // Latency & staleness assumptions.
    TimestampNs base_one_way_latency_ns{150'000};  // 150 us
    TimestampNs latency_jitter_ns{20'000};         // +/- 20 us
    TimestampNs stale_timeout_ns{5'000'000};       // 5 ms quote staleness timeout

    // Fill probability model assumptions.
    double base_fill_prob{0.92};
    double latency_decay_per_ms{0.18};  // Decay rate per ms of (latency + quote age)
    double depletion_sensitivity{0.30}; // Penalty when taking high fraction of displayed size

    // Venue order constraints.
    PriceTicks tick_size{100};  // $0.01 = 100 ticks
    Quantity lot_size{1};
    Quantity min_order_qty{1};
    Quantity max_child_qty{10'000};
    std::uint32_t max_active_children{64};
    double max_parent_share{1.0};  // Max fraction of a single parent order routed to this venue

    // Priority tie-breaker (lower is preferred when expected costs are equal).
    std::uint32_t priority{0};

    // Compute explicit taker fee per share in fractional ticks.
    [[nodiscard]] double taker_fee_ticks(PriceTicks raw_price) const noexcept {
        return static_cast<double>(taker_fee_per_share_ticks) +
               static_cast<double>(raw_price) * (taker_fee_bps / 10'000.0);
    }

    // Fee-adjusted effective price in fractional ticks:
    // For BUY: raw_price + taker_fee (higher fee = worse effective buy price).
    // For SELL: raw_price - taker_fee (higher fee = lower net proceeds = worse sell price).
    [[nodiscard]] double effective_price_ticks(Side side, PriceTicks raw_price) const noexcept {
        const double fee = taker_fee_ticks(raw_price);
        return side == Side::Buy ? (static_cast<double>(raw_price) + fee)
                                 : (static_cast<double>(raw_price) - fee);
    }

    // Compute exact cash fee in USD for a fill.
    [[nodiscard]] double taker_fee_usd(PriceTicks fill_price, Quantity qty) const noexcept {
        const double fee_per_share_usd =
            taker_fee_ticks(fill_price) / static_cast<double>(kPriceScale);
        return fee_per_share_usd * static_cast<double>(qty);
    }
};

// Explicit, auditable cost decomposition for a candidate venue level (in bps relative to reference mid).
struct CostBreakdown {
    double raw_price_cost_bps{0.0};        // Half-spread / price deviation from arrival mid
    double explicit_fee_bps{0.0};          // Explicit venue taker fee (negative = rebate)
    double latency_risk_bps{0.0};          // Adverse selection / drift risk from latency + quote age
    double miss_penalty_bps{0.0};          // Expected re-routing penalty = (1 - p_fill) * miss_cost
    double estimated_fill_prob{1.0};       // Modelled fill probability in [0, 1]
    double effective_price_ticks{0.0};     // Fee-adjusted effective price in ticks
    double total_expected_cost_bps{0.0};   // Sum of the four cost terms
};

struct RouterConfig {
    RoutingPolicy policy{RoutingPolicy::SmartCostAware};
    double adverse_selection_bps_per_ms{0.8};  // Drift / adverse selection cost per ms of latency+age
    double miss_penalty_bps{2.5};              // Re-routing cost in bps when an order misses
    double stale_age_weight{0.5};              // Weight of quote age relative to one-way latency
    bool check_limit_on_effective_price{false};
    bool respect_staleness{true};
    bool respect_venue_health{true};
    bool allow_partial_sweep{true};
    std::uint32_t max_reroute_attempts{3};
};

}  // namespace sor
