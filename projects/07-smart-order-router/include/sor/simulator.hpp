#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sor/audit_log.hpp"
#include "sor/config.hpp"
#include "sor/types.hpp"

namespace sor {

struct SimulationMetrics {
    std::string scenario_name{};
    std::string policy_name{};
    std::uint64_t seed{0};

    std::uint32_t parent_orders{0};
    std::uint32_t filled_orders{0};
    std::uint32_t partial_orders{0};
    std::uint32_t unfilled_orders{0};
    std::uint32_t risk_rejected_orders{0};

    Quantity total_requested_qty{0};
    Quantity total_filled_qty{0};
    double fill_ratio{0.0};

    // Implementation shortfall vs arrival consolidated mid (quantity-weighted, in bps).
    double gross_is_bps{0.0};
    double fee_cost_bps{0.0};
    double net_is_bps{0.0};
    double total_fees_usd{0.0};

    double median_parent_latency_us{0.0};
    double p99_parent_latency_us{0.0};

    std::uint64_t children_sent{0};
    std::uint64_t children_filled{0};
    std::uint64_t children_cancelled{0};
    std::uint64_t children_rejected{0};
    std::uint64_t children_timed_out{0};
    std::uint64_t failovers_triggered{0};
    double cancel_to_fill_ratio{0.0};

    std::size_t venue_count{0};
    std::array<std::string, kMaxVenues> venue_names{};
    std::array<Quantity, kMaxVenues> venue_fill_qty{};
    std::array<double, kMaxVenues> venue_fill_share{};
    std::array<std::uint64_t, kMaxVenues> venue_child_sent{};

    [[nodiscard]] std::string to_json() const;
};

struct SimulationOutput {
    SimulationMetrics metrics{};
    AuditLog audit_log{};
};

// Discrete-event multi-venue execution simulator with separate true venue books vs perceived
// router quotes, realistic in-flight latency, competing-flow queue depletion, partial fills,
// stale-feed freezes, and scripted venue outages.
class ExecutionSimulator {
public:
    static SimulationOutput run(const SystemConfig& base_cfg);
};

}  // namespace sor
