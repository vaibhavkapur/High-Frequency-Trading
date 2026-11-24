#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "sor/audit_log.hpp"
#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/oms.hpp"
#include "sor/quote_normalizer.hpp"
#include "sor/risk_manager.hpp"
#include "sor/route_planner.hpp"
#include "sor/simulator.hpp"
#include "sor/venue_health.hpp"

namespace {

void print_usage() {
    std::cout
        << "Multi-Venue Smart Order Router (sor)\n"
        << "Usage:\n"
        << "  sor --demo [--audit-out <path>]\n"
        << "  sor --simulate [options]\n"
        << "  sor --compare  [options]\n\n"
        << "Options:\n"
        << "  --config <path>            Load JSON configuration file\n"
        << "  --scenario <name>          fee_inversion | stale_feed_trap | latency_race_depletion |\n"
        << "                             venue_failover | mixed_stress\n"
        << "  --policy <name>            naive_primary | naive_best_price | fee_aware | smart_cost_aware\n"
        << "  --orders <N>               Number of parent orders to simulate (default: 200)\n"
        << "  --seed <S>                 PRNG seed (default: 42)\n"
        << "  --fee-scale <X>            Multiplier on venue fees (default: 1.0)\n"
        << "  --latency-scale <X>        Multiplier on venue latencies (default: 1.0)\n"
        << "  --liquidity-scale <X>      Multiplier on displayed book depth (default: 1.0)\n"
        << "  --stale-timeout-ms <X>     Override quote staleness timeout in ms\n"
        << "  --audit-out <path>         Write structured JSONL audit log\n"
        << "  --json                     Emit machine-readable JSON output\n"
        << "  --help                     Show this help message\n";
}

void print_metrics_table(const std::vector<sor::SimulationMetrics>& rows) {
    std::cout << std::left << std::setw(18) << "Policy"
              << std::right << std::setw(10) << "Fill%"
              << std::setw(11) << "GrossIS"
              << std::setw(10) << "FeeBps"
              << std::setw(10) << "NetIS"
              << std::setw(10) << "Cxl/Fill"
              << std::setw(10) << "Failover"
              << std::setw(12) << "MedLat(us)"
              << "  VenueShares\n";
    std::cout << std::string(115, '-') << "\n";
    for (const auto& m : rows) {
        std::cout << std::left << std::setw(18) << m.policy_name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(9) << (m.fill_ratio * 100.0) << "%"
                  << std::setprecision(2)
                  << std::setw(11) << m.gross_is_bps
                  << std::setw(10) << m.fee_cost_bps
                  << std::setw(10) << m.net_is_bps
                  << std::setw(10) << m.cancel_to_fill_ratio
                  << std::setw(10) << m.failovers_triggered
                  << std::setprecision(1)
                  << std::setw(12) << m.median_parent_latency_us << "  ";
        for (std::size_t i = 0; i < m.venue_count; ++i) {
            if (i > 0) std::cout << " ";
            std::cout << m.venue_names[i] << ":" << std::setprecision(0)
                      << (m.venue_fill_share[i] * 100.0) << "%";
        }
        std::cout << "\n";
    }
}

int run_interactive_demo(const std::string& audit_out) {
    using namespace sor;

    std::cout << "=== Multi-Venue Smart Order Router Demo ===\n";
    std::cout << "NOTE: Educational simulated execution engine (not a regulatory NBBO router).\n\n";

    SystemConfig cfg = make_default_config();
    cfg.router.policy = RoutingPolicy::SmartCostAware;

    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    RoutePlanner planner(cfg.router);
    RiskManager risk(cfg.risk);
    VenueHealthMonitor health(cfg.health);
    AuditLog audit;
    OrderManagementSystem oms(book, planner, risk, health, audit);

    const TimestampNs t0 = 10'000'000'000;  // 10.0s

    // 1) Populate 3-venue consolidated book where ARCA has slightly tighter raw ask ($150.0000)
    //    with +$0.0030 fee, BATS_INV has $150.0010 ask with -$0.0018 rebate (cheaper effective!),
    //    and IEX_SIM has $149.9950 ask that is STALE (12 ms old > 5 ms timeout).
    book.set_level(0, Side::Buy, usd_to_ticks(149.9900), 400, t0 - 200'000, t0 - 100'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.0000), 300, t0 - 200'000, t0 - 100'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.0100), 500, t0 - 200'000, t0 - 100'000);

    book.set_level(1, Side::Buy, usd_to_ticks(149.9900), 300, t0 - 300'000, t0 - 150'000);
    book.set_level(1, Side::Sell, usd_to_ticks(150.0010), 250, t0 - 300'000, t0 - 150'000);
    book.set_level(1, Side::Sell, usd_to_ticks(150.0100), 400, t0 - 300'000, t0 - 150'000);

    // Stale quote on IEX_SIM (12ms old > 5ms stale timeout).
    book.set_level(2, Side::Buy, usd_to_ticks(149.9800), 500, t0 - 12'500'000, t0 - 12'000'000);
    book.set_level(2, Side::Sell, usd_to_ticks(149.9950), 500, t0 - 12'500'000, t0 - 12'000'000);

    const auto bbo = book.consolidated_bbo(t0, &health, true, true);
    std::cout << "[1] Consolidated BBO (stale IEX_SIM excluded):\n"
              << std::fixed << std::setprecision(4)
              << "    Raw Best Bid: $" << ticks_to_usd(bbo.best_bid_price)
              << " | Raw Best Ask: $" << ticks_to_usd(bbo.best_ask_price)
              << " (" << cfg.venues[bbo.best_ask_venue].name << ")\n"
              << "    Fee-Adjusted Best Ask: $" << (bbo.best_eff_ask_ticks / kPriceScale)
              << " (" << cfg.venues[bbo.best_eff_ask_venue].name << " wins via -$0.0018 rebate)\n\n";

    // 2) Submit parent BUY 400 @ $150.05 IOC -> splits 250 to BATS_INV (cheaper all-in) + 150 to ARCA.
    std::vector<ChildCommand> cmds;
    ParentOrderRequest req1{};
    req1.client_order_id = "DEMO-PARENT-001";
    req1.symbol_id = 1;
    req1.side = Side::Buy;
    req1.quantity = 400;
    req1.limit_price = usd_to_ticks(150.0500);
    req1.tif = TimeInForce::IOC;
    req1.timestamp_ns = t0;

    const OrderId pid1 = oms.submit_parent(req1, cmds);
    std::cout << "[2] Submitted Parent " << req1.client_order_id
              << " (BUY 400 @ $150.05 IOC) -> " << cmds.size() << " child orders:\n";
    for (const auto& c : cmds) {
        std::cout << "    -> Child #" << c.child_id << " to " << cfg.venues[c.venue_id].name
                  << " qty=" << c.quantity << " limit=$" << ticks_to_usd(c.limit_price) << "\n";
    }

    // 3) Simulate partial fill on the first child (150 filled + duplicate exec_id ignored,
    //    remainder IOC-cancelled), full fill on the second child, and automatic reroute!
    std::vector<ChildCommand> follow_up;
    const auto& first_cmd = cmds[0];
    const auto& second_cmd = cmds[1];
    const Quantity partial_qty = first_cmd.quantity / 2;

    ExecutionReport fill_first{};
    fill_first.type = ExecReportType::Fill;
    fill_first.child_id = first_cmd.child_id;
    fill_first.venue_id = first_cmd.venue_id;
    fill_first.exec_id = 9001;
    fill_first.fill_qty = partial_qty;
    fill_first.fill_price = first_cmd.limit_price;
    fill_first.fee_usd = cfg.venues[first_cmd.venue_id].taker_fee_usd(fill_first.fill_price, partial_qty);
    fill_first.timestamp_ns = t0 + 250'000;
    oms.on_execution_report(fill_first, follow_up);
    oms.on_execution_report(fill_first, follow_up);  // Duplicate exec_id ignored!

    ExecutionReport fill_second{};
    fill_second.type = ExecReportType::Fill;
    fill_second.child_id = second_cmd.child_id;
    fill_second.venue_id = second_cmd.venue_id;
    fill_second.exec_id = 9002;
    fill_second.fill_qty = second_cmd.quantity;
    fill_second.fill_price = second_cmd.limit_price;
    fill_second.fee_usd =
        cfg.venues[second_cmd.venue_id].taker_fee_usd(fill_second.fill_price, second_cmd.quantity);
    fill_second.timestamp_ns = t0 + 450'000;
    oms.on_execution_report(fill_second, follow_up);

    // First venue cancels unfilled remainder -> OMS automatically reroutes remainder to other venue!
    ExecutionReport cxl_first{};
    cxl_first.type = ExecReportType::CancelAck;
    cxl_first.child_id = first_cmd.child_id;
    cxl_first.venue_id = first_cmd.venue_id;
    cxl_first.timestamp_ns = t0 + 460'000;
    oms.on_execution_report(cxl_first, follow_up);

    std::cout << "\n[3] After " << cfg.venues[first_cmd.venue_id].name << " partial fill ("
              << partial_qty << "/" << first_cmd.quantity
              << " + duplicate exec ignored) & IOC cancel:\n"
              << "    Automatic reroute emitted " << follow_up.size() << " child order(s):\n";
    for (const auto& c : follow_up) {
        std::cout << "    -> Rerouted Child #" << c.child_id << " to "
                  << cfg.venues[c.venue_id].name << " qty=" << c.quantity << " limit=$"
                  << ticks_to_usd(c.limit_price) << "\n";
        ExecutionReport final_fill{};
        final_fill.type = ExecReportType::Fill;
        final_fill.child_id = c.child_id;
        final_fill.venue_id = c.venue_id;
        final_fill.exec_id = 9003;
        final_fill.fill_qty = c.quantity;
        final_fill.fill_price = c.limit_price;
        final_fill.fee_usd = cfg.venues[c.venue_id].taker_fee_usd(final_fill.fill_price, c.quantity);
        final_fill.timestamp_ns = t0 + 700'000;
        std::vector<ChildCommand> empty;
        oms.on_execution_report(final_fill, empty);
    }

    const auto* p1 = oms.get_parent(pid1);
    std::cout << "    Parent final state: " << to_string(p1->state)
              << " | filled=" << p1->filled_qty << "/" << p1->orig_qty
              << " | avg_px=$" << p1->avg_fill_price_usd()
              << " | net_fees=$" << p1->total_fees_usd
              << " | net_IS=" << std::setprecision(2) << p1->net_shortfall_bps() << " bps\n"
              << "    Duplicate exec reports safely ignored: " << oms.stats().duplicate_execs_ignored
              << "\n\n";

    // 4) Duplicate client_order_id pre-trade risk check.
    std::vector<ChildCommand> dup_cmds;
    const OrderId pid_dup = oms.submit_parent(req1, dup_cmds);
    const auto* pdup = oms.get_parent(pid_dup);
    std::cout << "[4] Duplicate client_order_id submission:\n"
              << "    State=" << to_string(pdup->state)
              << " | RiskReason=" << to_string(pdup->risk_reason) << "\n\n";

    // 5) Controlled multi-policy backtest comparison on MixedStress scenario.
    std::cout << "[5] Multi-Policy Backtest Comparison (scenario=mixed_stress, 200 parent orders, seed=42):\n";
    std::vector<SimulationMetrics> comparison;
    for (RoutingPolicy pol : {RoutingPolicy::NaivePrimary, RoutingPolicy::NaiveBestPrice,
                              RoutingPolicy::FeeAware, RoutingPolicy::SmartCostAware}) {
        SystemConfig scfg = make_default_config();
        scfg.sim.scenario = ScenarioType::MixedStress;
        scfg.sim.num_parent_orders = 200;
        scfg.sim.seed = 42;
        scfg.router.policy = pol;
        comparison.push_back(ExecutionSimulator::run(scfg).metrics);
    }
    print_metrics_table(comparison);

    if (!audit_out.empty()) {
        if (audit.write_jsonl(audit_out)) {
            std::cout << "\nAudit log (" << audit.size() << " events) written to " << audit_out
                      << "\n";
        } else {
            std::cerr << "Failed to write audit log to " << audit_out << "\n";
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool mode_demo = false;
    bool mode_simulate = false;
    bool mode_compare = false;
    bool json_output = false;

    std::string config_path;
    std::string scenario_arg;
    std::string policy_arg;
    std::string audit_out;
    int orders_override = -1;
    std::uint64_t seed_override = 0;
    bool has_seed = false;
    double fee_scale = -1.0;
    double latency_scale = -1.0;
    double liquidity_scale = -1.0;
    double stale_timeout_ms = -1.0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--demo") {
            mode_demo = true;
        } else if (arg == "--simulate") {
            mode_simulate = true;
        } else if (arg == "--compare") {
            mode_compare = true;
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--scenario" && i + 1 < argc) {
            scenario_arg = argv[++i];
        } else if (arg == "--policy" && i + 1 < argc) {
            policy_arg = argv[++i];
        } else if (arg == "--orders" && i + 1 < argc) {
            orders_override = std::atoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed_override = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
            has_seed = true;
        } else if (arg == "--fee-scale" && i + 1 < argc) {
            fee_scale = std::atof(argv[++i]);
        } else if (arg == "--latency-scale" && i + 1 < argc) {
            latency_scale = std::atof(argv[++i]);
        } else if (arg == "--liquidity-scale" && i + 1 < argc) {
            liquidity_scale = std::atof(argv[++i]);
        } else if (arg == "--stale-timeout-ms" && i + 1 < argc) {
            stale_timeout_ms = std::atof(argv[++i]);
        } else if (arg == "--audit-out" && i + 1 < argc) {
            audit_out = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    if (!mode_demo && !mode_simulate && !mode_compare) {
        mode_demo = true;
    }

    if (mode_demo) {
        return run_interactive_demo(audit_out);
    }

    sor::SystemConfig cfg = sor::make_default_config();
    if (!config_path.empty()) {
        std::string err;
        if (!sor::load_config_from_file(config_path, cfg, err)) {
            std::cerr << "Config error: " << err << "\n";
            return 1;
        }
    }

    if (!scenario_arg.empty()) {
        const auto sc = sor::parse_scenario(scenario_arg);
        if (!sc) {
            std::cerr << "Invalid scenario: " << scenario_arg << "\n";
            return 1;
        }
        cfg.sim.scenario = *sc;
    }
    if (!policy_arg.empty()) {
        const auto pol = sor::parse_routing_policy(policy_arg);
        if (!pol) {
            std::cerr << "Invalid policy: " << policy_arg << "\n";
            return 1;
        }
        cfg.router.policy = *pol;
    }
    if (orders_override > 0) cfg.sim.num_parent_orders = static_cast<std::uint32_t>(orders_override);
    if (has_seed) cfg.sim.seed = seed_override;
    if (fee_scale > 0.0) cfg.sim.fee_scale = fee_scale;
    if (latency_scale > 0.0) cfg.sim.latency_scale = latency_scale;
    if (liquidity_scale > 0.0) cfg.sim.liquidity_scale = liquidity_scale;
    if (stale_timeout_ms > 0.0) {
        cfg.sim.stale_timeout_override_ns =
            static_cast<sor::TimestampNs>(stale_timeout_ms * 1'000'000.0);
    }
    if (!audit_out.empty()) cfg.sim.record_audit_log = true;

    if (mode_simulate) {
        auto out = sor::ExecutionSimulator::run(cfg);
        if (!audit_out.empty()) {
            if (!out.audit_log.write_jsonl(audit_out)) {
                std::cerr << "Failed to write audit log to " << audit_out << "\n";
                return 1;
            }
        }
        if (json_output) {
            std::cout << out.metrics.to_json() << "\n";
        } else {
            std::cout << "Scenario: " << out.metrics.scenario_name
                      << " | Seed: " << out.metrics.seed
                      << " | Orders: " << out.metrics.parent_orders << "\n";
            print_metrics_table({out.metrics});
        }
        return 0;
    }

    // --compare mode: run all four routing policies on the exact same scenario & seed.
    std::vector<sor::SimulationMetrics> rows;
    for (sor::RoutingPolicy pol :
         {sor::RoutingPolicy::NaivePrimary, sor::RoutingPolicy::NaiveBestPrice,
          sor::RoutingPolicy::FeeAware, sor::RoutingPolicy::SmartCostAware}) {
        sor::SystemConfig pcfg = cfg;
        pcfg.router.policy = pol;
        pcfg.sim.record_audit_log = false;
        rows.push_back(sor::ExecutionSimulator::run(pcfg).metrics);
    }

    if (json_output) {
        std::cout << "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i > 0) std::cout << ",";
            std::cout << rows[i].to_json();
        }
        std::cout << "]\n";
    } else {
        std::cout << "Scenario: " << sor::to_string(cfg.sim.scenario)
                  << " | Seed: " << cfg.sim.seed
                  << " | Orders: " << cfg.sim.num_parent_orders << "\n";
        print_metrics_table(rows);
    }
    return 0;
}
