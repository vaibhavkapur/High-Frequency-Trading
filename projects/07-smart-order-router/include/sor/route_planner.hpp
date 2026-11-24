#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sor/consolidated_book.hpp"
#include "sor/types.hpp"
#include "sor/venue_health.hpp"

namespace sor {

inline constexpr std::size_t kMaxRouteSlices = 32;

struct RouteRequest {
    OrderId parent_id{0};
    Side side{Side::Buy};
    Quantity qty{0};
    PriceTicks limit_price{0};
    TimeInForce tif{TimeInForce::IOC};
    TimestampNs now_ns{0};
    double reference_mid_ticks{0.0};  // If <= 0, derived from consolidated BBO.
    std::uint32_t excluded_venues_mask{0};
    std::array<std::uint32_t, kMaxVenues> active_children_per_venue{};
};

struct PlannedChild {
    VenueId venue_id{kInvalidVenue};
    PriceTicks limit_price{kInvalidPrice};
    Quantity quantity{0};
    TimestampNs quote_age_ns{0};
    double estimated_latency_ns{0.0};
    CostBreakdown cost{};
};

struct VenueEvaluation {
    VenueId venue_id{kInvalidVenue};
    IneligibilityReason reason{IneligibilityReason::Eligible};
    PriceTicks best_raw_price{kInvalidPrice};
    Quantity displayed_qty_at_best{0};
    TimestampNs quote_age_ns{0};
    CostBreakdown best_cost{};
};

struct RoutePlan {
    std::array<PlannedChild, kMaxRouteSlices> slices{};
    std::size_t slice_count{0};

    std::array<VenueEvaluation, kMaxVenues> venue_evals{};
    std::size_t eval_count{0};

    Quantity requested_qty{0};
    Quantity planned_qty{0};
    Quantity unroutable_qty{0};
    double reference_mid_ticks{0.0};
    double expected_all_in_cost_bps{0.0};
    bool fok_failed{false};

    [[nodiscard]] bool empty() const noexcept { return slice_count == 0; }
};

class RoutePlanner {
public:
    explicit RoutePlanner(RouterConfig cfg = {}) noexcept : cfg_(cfg) {}

    void set_config(RouterConfig cfg) noexcept { cfg_ = cfg; }
    [[nodiscard]] const RouterConfig& config() const noexcept { return cfg_; }

    // Zero-heap-allocation route planning over the current consolidated L2 book.
    [[nodiscard]] RoutePlan plan(const ConsolidatedBook& book,
                                 const VenueHealthMonitor* health,
                                 const RouteRequest& request) const noexcept;

    // Human-readable explanation of the routing decision and per-venue cost breakdown.
    [[nodiscard]] static std::string format_explanation(const RoutePlan& plan,
                                                        const ConsolidatedBook& book);

private:
    RouterConfig cfg_{};
};

}  // namespace sor
