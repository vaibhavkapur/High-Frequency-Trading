#include "sor/config.hpp"

#include <fstream>
#include <sstream>

#include "sor/json_util.hpp"

namespace sor {

std::string_view to_string(ScenarioType s) noexcept {
    switch (s) {
        case ScenarioType::FeeInversion: return "fee_inversion";
        case ScenarioType::StaleFeedTrap: return "stale_feed_trap";
        case ScenarioType::LatencyRaceDepletion: return "latency_race_depletion";
        case ScenarioType::VenueFailover: return "venue_failover";
        case ScenarioType::MixedStress: return "mixed_stress";
    }
    return "unknown";
}

std::optional<ScenarioType> parse_scenario(std::string_view s) noexcept {
    if (s == "fee_inversion") return ScenarioType::FeeInversion;
    if (s == "stale_feed_trap") return ScenarioType::StaleFeedTrap;
    if (s == "latency_race_depletion") return ScenarioType::LatencyRaceDepletion;
    if (s == "venue_failover") return ScenarioType::VenueFailover;
    if (s == "mixed_stress") return ScenarioType::MixedStress;
    return std::nullopt;
}

std::optional<RoutingPolicy> parse_routing_policy(std::string_view s) noexcept {
    if (s == "naive_primary") return RoutingPolicy::NaivePrimary;
    if (s == "naive_best_price") return RoutingPolicy::NaiveBestPrice;
    if (s == "fee_aware") return RoutingPolicy::FeeAware;
    if (s == "smart_cost_aware") return RoutingPolicy::SmartCostAware;
    return std::nullopt;
}

SystemConfig make_default_config() {
    SystemConfig cfg{};

    // Venue 0: ARCA-like maker-taker venue (deep, fast, standard $0.0030/share taker fee).
    VenueConfig arca{};
    arca.id = 0;
    arca.name = "ARCA";
    arca.enabled = true;
    arca.taker_fee_per_share_ticks = 30;  // +$0.0030/share
    arca.maker_fee_per_share_ticks = -20;
    arca.base_one_way_latency_ns = 120'000;  // 120 us
    arca.latency_jitter_ns = 15'000;
    arca.stale_timeout_ns = 5'000'000;  // 5 ms
    arca.base_fill_prob = 0.94;
    arca.latency_decay_per_ms = 0.16;
    arca.depletion_sensitivity = 0.25;
    arca.tick_size = 100;
    arca.lot_size = 1;
    arca.min_order_qty = 1;
    arca.max_child_qty = 5'000;
    arca.max_active_children = 64;
    arca.priority = 0;

    // Venue 1: Inverted venue (taker rebate -$0.0018/share, slightly slower, moderate depth).
    VenueConfig bats{};
    bats.id = 1;
    bats.name = "BATS_INV";
    bats.enabled = true;
    bats.taker_fee_per_share_ticks = -18;  // -$0.0018/share rebate
    bats.maker_fee_per_share_ticks = 22;
    bats.base_one_way_latency_ns = 220'000;  // 220 us
    bats.latency_jitter_ns = 25'000;
    bats.stale_timeout_ns = 5'000'000;
    bats.base_fill_prob = 0.89;
    bats.latency_decay_per_ms = 0.20;
    bats.depletion_sensitivity = 0.30;
    bats.tick_size = 100;
    bats.lot_size = 1;
    bats.min_order_qty = 1;
    bats.max_child_qty = 3'000;
    bats.max_active_children = 64;
    bats.priority = 1;

    // Venue 2: Speed-bump / low-fee venue (low $0.0009/share taker fee, higher 450 us latency).
    VenueConfig iex{};
    iex.id = 2;
    iex.name = "IEX_SIM";
    iex.enabled = true;
    iex.taker_fee_per_share_ticks = 9;  // +$0.0009/share
    iex.maker_fee_per_share_ticks = 0;
    iex.base_one_way_latency_ns = 450'000;  // 450 us
    iex.latency_jitter_ns = 40'000;
    iex.stale_timeout_ns = 5'000'000;
    iex.base_fill_prob = 0.84;
    iex.latency_decay_per_ms = 0.24;
    iex.depletion_sensitivity = 0.35;
    iex.tick_size = 100;
    iex.lot_size = 1;
    iex.min_order_qty = 1;
    iex.max_child_qty = 2'000;
    iex.max_active_children = 64;
    iex.priority = 2;

    cfg.venues = {arca, bats, iex};
    cfg.router = RouterConfig{};
    cfg.risk = RiskLimits{};
    cfg.health = VenueHealthConfig{};
    cfg.sim = SimulationConfig{};
    return cfg;
}

bool load_config_from_json(std::string_view json_text, SystemConfig& out, std::string& error_out) {
    JsonValue root;
    if (!parse_json(json_text, root, error_out)) {
        return false;
    }
    if (!root.is_object()) {
        error_out = "config root must be a JSON object";
        return false;
    }

    SystemConfig cfg = make_default_config();

    if (const auto* venues_val = root.find("venues")) {
        if (!venues_val->is_array() || venues_val->as_array().empty()) {
            error_out = "'venues' must be a non-empty array";
            return false;
        }
        if (venues_val->as_array().size() > kMaxVenues) {
            error_out = "'venues' exceeds kMaxVenues";
            return false;
        }
        cfg.venues.clear();
        VenueId auto_id = 0;
        for (const auto& item : venues_val->as_array()) {
            if (!item.is_object()) {
                error_out = "each venue entry must be an object";
                return false;
            }
            VenueConfig v{};
            v.id = auto_id++;
            if (const auto* idv = item.find("id"); idv && idv->is_number()) {
                const auto raw_id = idv->as_i64(-1);
                if (raw_id < 0 || raw_id >= static_cast<std::int64_t>(kMaxVenues)) {
                    error_out = "venue id out of range";
                    return false;
                }
                v.id = static_cast<VenueId>(raw_id);
            }
            if (const auto* nv = item.find("name"); nv && nv->is_string()) v.name = nv->as_string();
            if (const auto* ev = item.find("enabled"); ev && ev->is_bool()) v.enabled = ev->as_bool();
            if (const auto* f = item.find("taker_fee_per_share_ticks"); f && f->is_number())
                v.taker_fee_per_share_ticks = f->as_i64();
            if (const auto* f = item.find("taker_fee_bps"); f && f->is_number())
                v.taker_fee_bps = f->as_double();
            if (const auto* f = item.find("maker_fee_per_share_ticks"); f && f->is_number())
                v.maker_fee_per_share_ticks = f->as_i64();
            if (const auto* f = item.find("base_one_way_latency_ns"); f && f->is_number())
                v.base_one_way_latency_ns = f->as_i64();
            if (const auto* f = item.find("latency_jitter_ns"); f && f->is_number())
                v.latency_jitter_ns = f->as_i64();
            if (const auto* f = item.find("stale_timeout_ns"); f && f->is_number())
                v.stale_timeout_ns = f->as_i64();
            if (const auto* f = item.find("base_fill_prob"); f && f->is_number())
                v.base_fill_prob = f->as_double();
            if (const auto* f = item.find("latency_decay_per_ms"); f && f->is_number())
                v.latency_decay_per_ms = f->as_double();
            if (const auto* f = item.find("depletion_sensitivity"); f && f->is_number())
                v.depletion_sensitivity = f->as_double();
            if (const auto* f = item.find("tick_size"); f && f->is_number())
                v.tick_size = f->as_i64();
            if (const auto* f = item.find("lot_size"); f && f->is_number())
                v.lot_size = f->as_i64();
            if (const auto* f = item.find("min_order_qty"); f && f->is_number())
                v.min_order_qty = f->as_i64();
            if (const auto* f = item.find("max_child_qty"); f && f->is_number())
                v.max_child_qty = f->as_i64();
            if (const auto* f = item.find("max_active_children"); f && f->is_number())
                v.max_active_children = static_cast<std::uint32_t>(f->as_i64());
            if (const auto* f = item.find("max_parent_share"); f && f->is_number())
                v.max_parent_share = f->as_double();
            if (const auto* f = item.find("priority"); f && f->is_number())
                v.priority = static_cast<std::uint32_t>(f->as_i64());

            if (v.lot_size <= 0 || v.min_order_qty <= 0 || v.max_child_qty < v.min_order_qty) {
                error_out = "invalid venue quantity constraints for " + v.name;
                return false;
            }
            cfg.venues.push_back(std::move(v));
        }
    }

    if (const auto* rv = root.find("router"); rv && rv->is_object()) {
        if (const auto* pv = rv->find("policy"); pv && pv->is_string()) {
            const auto parsed = parse_routing_policy(pv->as_string());
            if (!parsed) {
                error_out = "unknown routing policy: " + pv->as_string();
                return false;
            }
            cfg.router.policy = *parsed;
        }
        if (const auto* f = rv->find("adverse_selection_bps_per_ms"); f && f->is_number())
            cfg.router.adverse_selection_bps_per_ms = f->as_double();
        if (const auto* f = rv->find("miss_penalty_bps"); f && f->is_number())
            cfg.router.miss_penalty_bps = f->as_double();
        if (const auto* f = rv->find("stale_age_weight"); f && f->is_number())
            cfg.router.stale_age_weight = f->as_double();
        if (const auto* f = rv->find("check_limit_on_effective_price"); f && f->is_bool())
            cfg.router.check_limit_on_effective_price = f->as_bool();
        if (const auto* f = rv->find("respect_staleness"); f && f->is_bool())
            cfg.router.respect_staleness = f->as_bool();
        if (const auto* f = rv->find("respect_venue_health"); f && f->is_bool())
            cfg.router.respect_venue_health = f->as_bool();
        if (const auto* f = rv->find("allow_partial_sweep"); f && f->is_bool())
            cfg.router.allow_partial_sweep = f->as_bool();
        if (const auto* f = rv->find("max_reroute_attempts"); f && f->is_number())
            cfg.router.max_reroute_attempts = static_cast<std::uint32_t>(f->as_i64());
    }

    if (const auto* rk = root.find("risk"); rk && rk->is_object()) {
        if (const auto* f = rk->find("max_order_qty"); f && f->is_number())
            cfg.risk.max_order_qty = f->as_i64();
        if (const auto* f = rk->find("max_order_notional_usd"); f && f->is_number())
            cfg.risk.max_order_notional_usd = f->as_double();
        if (const auto* f = rk->find("max_net_position_qty"); f && f->is_number())
            cfg.risk.max_net_position_qty = f->as_i64();
        if (const auto* f = rk->find("max_gross_position_qty"); f && f->is_number())
            cfg.risk.max_gross_position_qty = f->as_i64();
        if (const auto* f = rk->find("max_open_notional_usd"); f && f->is_number())
            cfg.risk.max_open_notional_usd = f->as_double();
        if (const auto* f = rk->find("max_orders_per_second"); f && f->is_number())
            cfg.risk.max_orders_per_second = static_cast<std::uint32_t>(f->as_i64());
        if (const auto* f = rk->find("max_price_collar_bps"); f && f->is_number())
            cfg.risk.max_price_collar_bps = f->as_double();
    }

    if (const auto* hv = root.find("health"); hv && hv->is_object()) {
        if (const auto* f = hv->find("degraded_reject_threshold"); f && f->is_number())
            cfg.health.degraded_reject_threshold = static_cast<std::uint32_t>(f->as_i64());
        if (const auto* f = hv->find("circuit_open_reject_threshold"); f && f->is_number())
            cfg.health.circuit_open_reject_threshold = static_cast<std::uint32_t>(f->as_i64());
        if (const auto* f = hv->find("circuit_open_timeout_threshold"); f && f->is_number())
            cfg.health.circuit_open_timeout_threshold = static_cast<std::uint32_t>(f->as_i64());
        if (const auto* f = hv->find("recovery_cooldown_ns"); f && f->is_number())
            cfg.health.recovery_cooldown_ns = f->as_i64();
    }

    if (const auto* sv = root.find("simulation"); sv && sv->is_object()) {
        if (const auto* sc = sv->find("scenario"); sc && sc->is_string()) {
            const auto parsed = parse_scenario(sc->as_string());
            if (!parsed) {
                error_out = "unknown simulation scenario: " + sc->as_string();
                return false;
            }
            cfg.sim.scenario = *parsed;
        }
        if (const auto* f = sv->find("seed"); f && f->is_number())
            cfg.sim.seed = static_cast<std::uint64_t>(f->as_i64());
        if (const auto* f = sv->find("num_parent_orders"); f && f->is_number())
            cfg.sim.num_parent_orders = static_cast<std::uint32_t>(f->as_i64());
        if (const auto* f = sv->find("min_parent_qty"); f && f->is_number())
            cfg.sim.min_parent_qty = f->as_i64();
        if (const auto* f = sv->find("max_parent_qty"); f && f->is_number())
            cfg.sim.max_parent_qty = f->as_i64();
        if (const auto* f = sv->find("initial_mid_ticks"); f && f->is_number())
            cfg.sim.initial_mid_ticks = f->as_i64();
        if (const auto* f = sv->find("liquidity_scale"); f && f->is_number())
            cfg.sim.liquidity_scale = f->as_double();
        if (const auto* f = sv->find("latency_scale"); f && f->is_number())
            cfg.sim.latency_scale = f->as_double();
        if (const auto* f = sv->find("fee_scale"); f && f->is_number())
            cfg.sim.fee_scale = f->as_double();
        if (const auto* f = sv->find("depletion_rate"); f && f->is_number())
            cfg.sim.depletion_rate = f->as_double();
    }

    out = std::move(cfg);
    return true;
}

bool load_config_from_file(const std::string& path, SystemConfig& out, std::string& error_out) {
    std::ifstream ifs(path);
    if (!ifs) {
        error_out = "unable to open config file: " + path;
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return load_config_from_json(oss.str(), out, error_out);
}

}  // namespace sor
