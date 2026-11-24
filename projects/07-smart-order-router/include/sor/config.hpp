#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sor/risk_manager.hpp"
#include "sor/types.hpp"
#include "sor/venue_health.hpp"

namespace sor {

enum class ScenarioType : std::uint8_t {
    FeeInversion = 0,
    StaleFeedTrap = 1,
    LatencyRaceDepletion = 2,
    VenueFailover = 3,
    MixedStress = 4,
};

[[nodiscard]] std::string_view to_string(ScenarioType s) noexcept;
[[nodiscard]] std::optional<ScenarioType> parse_scenario(std::string_view s) noexcept;
[[nodiscard]] std::optional<RoutingPolicy> parse_routing_policy(std::string_view s) noexcept;

struct SimulationConfig {
    ScenarioType scenario{ScenarioType::MixedStress};
    std::uint64_t seed{42};
    std::uint32_t num_parent_orders{200};
    Quantity min_parent_qty{100};
    Quantity max_parent_qty{1'200};
    PriceTicks initial_mid_ticks{1'500'000};  // $150.00
    double liquidity_scale{1.0};
    double latency_scale{1.0};
    double fee_scale{1.0};
    double depletion_rate{0.35};  // Competing flow intensity per ms
    TimestampNs stale_timeout_override_ns{0};
    bool record_audit_log{false};
};

struct SystemConfig {
    std::vector<VenueConfig> venues{};
    RouterConfig router{};
    RiskLimits risk{};
    VenueHealthConfig health{};
    SimulationConfig sim{};
};

// Default 3-venue simulated equities-style setup (ARCA, BATS_INV, IEX_SIM).
[[nodiscard]] SystemConfig make_default_config();

[[nodiscard]] bool load_config_from_json(std::string_view json_text, SystemConfig& out,
                                         std::string& error_out);
[[nodiscard]] bool load_config_from_file(const std::string& path, SystemConfig& out,
                                         std::string& error_out);

}  // namespace sor
