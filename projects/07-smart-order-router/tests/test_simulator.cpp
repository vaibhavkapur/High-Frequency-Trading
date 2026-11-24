#include <gtest/gtest.h>

#include "sor/config.hpp"
#include "sor/simulator.hpp"

using namespace sor;

TEST(SimulatorTest, DeterministicReplayWithSameSeed) {
    SystemConfig cfg = make_default_config();
    cfg.sim.scenario = ScenarioType::MixedStress;
    cfg.sim.num_parent_orders = 60;
    cfg.sim.seed = 12345;
    cfg.router.policy = RoutingPolicy::SmartCostAware;

    const auto run1 = ExecutionSimulator::run(cfg);
    const auto run2 = ExecutionSimulator::run(cfg);

    EXPECT_EQ(run1.metrics.total_filled_qty, run2.metrics.total_filled_qty);
    EXPECT_EQ(run1.metrics.children_sent, run2.metrics.children_sent);
    EXPECT_DOUBLE_EQ(run1.metrics.net_is_bps, run2.metrics.net_is_bps);
    EXPECT_DOUBLE_EQ(run1.metrics.total_fees_usd, run2.metrics.total_fees_usd);
    EXPECT_EQ(run1.metrics.to_json(), run2.metrics.to_json());
}

TEST(SimulatorTest, FeeAwareBeatsNaiveBestPriceInFeeInversion) {
    SystemConfig base = make_default_config();
    base.sim.scenario = ScenarioType::FeeInversion;
    base.sim.num_parent_orders = 120;
    base.sim.seed = 77;

    auto naive_cfg = base;
    naive_cfg.router.policy = RoutingPolicy::NaiveBestPrice;
    const auto naive = ExecutionSimulator::run(naive_cfg).metrics;

    auto smart_cfg = base;
    smart_cfg.router.policy = RoutingPolicy::SmartCostAware;
    const auto smart = ExecutionSimulator::run(smart_cfg).metrics;

    EXPECT_GT(smart.fill_ratio, 0.90);
    EXPECT_LT(smart.net_is_bps, naive.net_is_bps);
    EXPECT_LT(smart.fee_cost_bps, naive.fee_cost_bps);
}

TEST(SimulatorTest, SmartRouterAvoidsStaleFeedTrap) {
    SystemConfig base = make_default_config();
    base.sim.scenario = ScenarioType::StaleFeedTrap;
    base.sim.num_parent_orders = 100;
    base.sim.seed = 99;

    auto naive_cfg = base;
    naive_cfg.router.policy = RoutingPolicy::NaiveBestPrice;
    const auto naive = ExecutionSimulator::run(naive_cfg).metrics;

    auto smart_cfg = base;
    smart_cfg.router.policy = RoutingPolicy::SmartCostAware;
    const auto smart = ExecutionSimulator::run(smart_cfg).metrics;

    EXPECT_GT(smart.fill_ratio, 0.95);
    EXPECT_LT(smart.cancel_to_fill_ratio, naive.cancel_to_fill_ratio);
    EXPECT_LT(smart.median_parent_latency_us, naive.median_parent_latency_us);
}

TEST(SimulatorTest, SmartRouterAvoidsSlowDepletingVenue) {
    SystemConfig base = make_default_config();
    base.sim.scenario = ScenarioType::LatencyRaceDepletion;
    base.sim.num_parent_orders = 120;
    base.sim.seed = 42;

    auto naive_cfg = base;
    naive_cfg.router.policy = RoutingPolicy::NaiveBestPrice;
    const auto naive = ExecutionSimulator::run(naive_cfg).metrics;

    auto smart_cfg = base;
    smart_cfg.router.policy = RoutingPolicy::SmartCostAware;
    const auto smart = ExecutionSimulator::run(smart_cfg).metrics;

    EXPECT_GE(smart.fill_ratio, naive.fill_ratio);
    EXPECT_LT(smart.cancel_to_fill_ratio, naive.cancel_to_fill_ratio * 0.5);
    EXPECT_LT(smart.median_parent_latency_us, naive.median_parent_latency_us * 0.5);
}

TEST(SimulatorTest, SmartRouterHandlesVenueFailover) {
    SystemConfig base = make_default_config();
    base.sim.scenario = ScenarioType::VenueFailover;
    base.sim.num_parent_orders = 100;
    base.sim.seed = 314;

    auto prim_cfg = base;
    prim_cfg.router.policy = RoutingPolicy::NaivePrimary;
    const auto prim = ExecutionSimulator::run(prim_cfg).metrics;

    auto smart_cfg = base;
    smart_cfg.router.policy = RoutingPolicy::SmartCostAware;
    const auto smart = ExecutionSimulator::run(smart_cfg).metrics;

    EXPECT_GT(smart.fill_ratio, prim.fill_ratio + 0.20);
    EXPECT_GT(smart.fill_ratio, 0.90);
}
