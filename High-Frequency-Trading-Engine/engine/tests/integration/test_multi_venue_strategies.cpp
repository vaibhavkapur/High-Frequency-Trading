#include "hft/adapters/arb_adapter.hpp"
#include "hft/adapters/router_adapter.hpp"
#include "hft/adapters/strategy_adapter.hpp"
#include "hft/runtime/pipeline.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

HFT_TEST(SmartOrderRouterFeeStalenessAndFailoverAcrossVenues) {
  const std::vector<InstrumentMeta> insts = {
      InstrumentMeta::make(0, "AAPL", 100, 1),
      InstrumentMeta::make(1, "MSFT", 100, 1),
  };
  std::vector<VenueMeta> venues(3);
  // Venue 0: high taker fee (+3.0 bps = 300 bps_x100)
  venues[0] = VenueMeta{0, "EXPENSIVE_V0", -20, 300, 50'000, 80'000, 50'000, 5'000'000, true};
  // Venue 1: inverted taker rebate (-1.5 bps = -150 bps_x100)
  venues[1] = VenueMeta{1, "INVERTED_V1", 20, -150, 50'000, 80'000, 50'000, 5'000'000, true};
  // Venue 2: slow venue
  venues[2] = VenueMeta{2, "SLOW_V2", 0, 100, 200'000, 400'000, 200'000, 5'000'000, true};

  SmartRouterAdapter sor_ad(insts, venues, sor::RoutingPolicy::FeeAware);
  const SimTimeNs t0{10'000'000LL};

  // Identical displayed ask $100.01 (10001 ticks) across Venue 0 and Venue 1.
  // FeeAware must pick Venue 1 (inverted rebate) over Venue 0 (taker fee)!
  sor_ad.update_venue_depth(0, 0, {{Price{9999}, Quantity{100}, 1}},
                            {{Price{10001}, Quantity{100}, 1}}, t0, t0);
  sor_ad.update_venue_depth(0, 1, {{Price{9999}, Quantity{100}, 1}},
                            {{Price{10001}, Quantity{100}, 1}}, t0, t0);
  sor_ad.update_venue_depth(0, 2, {{Price{9999}, Quantity{100}, 1}},
                            {{Price{10002}, Quantity{100}, 1}}, t0, t0);

  // Instrument 1 has completely distinct price ($200.00) -> verify multi-symbol isolation!
  sor_ad.update_venue_depth(1, 0, {{Price{19999}, Quantity{50}, 1}},
                            {{Price{20001}, Quantity{50}, 1}}, t0, t0);
  HFT_EXPECT_EQ(sor_ad.consolidated_bbo(0, t0).best_ask_price, 10001 * 100);
  HFT_EXPECT_EQ(sor_ad.consolidated_bbo(1, t0).best_ask_price, 20001 * 100);

  OrderIntent parent{};
  parent.action = OrderAction::New;
  parent.client_order_id = 1;
  parent.token = ClientToken::from_uint64("PAR", 1);
  parent.side = Side::Buy;
  parent.order_type = OrderType::Limit;
  parent.tif = TimeInForce::IOC;
  parent.limit_price = Price{10002};
  parent.qty = Quantity{50};

  std::vector<RoutedChildOrder> children;
  const ParentOrderId pid = sor_ad.route_parent(0, parent, t0, children);
  HFT_EXPECT_TRUE(pid != 0);
  HFT_EXPECT_EQ(children.size(), 1U);
  HFT_EXPECT_EQ(children[0].venue, 1U);  // inverted rebate venue chosen!

  // Simulate Venue 1 rejecting child -> SOR must failover!
  // Venue 0 displays $100.01 + 3.0 bps fee = $100.04 effective, whereas Venue 2 displays
  // $100.02 + 1.0 bps fee = $100.03 effective -> FeeAware picks Venue 2 over Venue 0!
  ExecutionReport rej{};
  rej.type = ExecReportType::Rejected;
  rej.token = children[0].intent.token;
  rej.reject_reason = RejectReason::ExchangeRejected;
  std::vector<RoutedChildOrder> reroutes;
  sor_ad.on_child_execution_report(0, 1, rej, t0 + 100'000LL, reroutes);
  HFT_EXPECT_TRUE(!reroutes.empty());
  HFT_EXPECT_EQ(reroutes[0].venue, 2U);

  // Fill rerouted child on Venue 2 -> Parent must transition to Filled with exact qty 50!
  ExecutionReport fill{};
  fill.type = ExecReportType::Filled;
  fill.token = reroutes[0].intent.token;
  fill.exec_id = 901;
  fill.fill_qty = Quantity{50};
  fill.exec_price = Price{10002};
  std::vector<RoutedChildOrder> none;
  sor_ad.on_child_execution_report(0, 2, fill, t0 + 200'000LL, none);
  const auto* p = sor_ad.find_parent(0, pid);
  HFT_EXPECT_TRUE(p != nullptr);
  HFT_EXPECT_EQ(p->state, sor::ParentOrderState::Filled);
  HFT_EXPECT_EQ(p->filled_qty, 50LL);

  // Staleness check: advance time past Venue 2's 5ms stale threshold while refreshing Venue 0 ->
  // Venue 2 is excluded as stale and Venue 0 is routed instead!
  const SimTimeNs t_stale = t0 + 10'000'000LL;  // +10 ms
  sor_ad.update_venue_depth(0, 0, {{Price{9999}, Quantity{100}, 1}},
                            {{Price{10001}, Quantity{100}, 1}}, t_stale, t_stale);
  OrderIntent parent2 = parent;
  parent2.client_order_id = 2;
  parent2.token = ClientToken::from_uint64("PAR", 2);
  std::vector<RoutedChildOrder> children2;
  const ParentOrderId pid2 = sor_ad.route_parent(0, parent2, t_stale, children2);
  HFT_EXPECT_TRUE(pid2 != 0);
  HFT_EXPECT_EQ(children2.size(), 1U);
  HFT_EXPECT_EQ(children2[0].venue, 0U);
}

HFT_TEST(MarketMakerPluginInventorySkewAndMultiVenueReconciliation) {
  const std::vector<InstrumentMeta> insts = {
      InstrumentMeta::make(0, "AAPL", 100, 1),
      InstrumentMeta::make(1, "MSFT", 100, 1),
  };
  std::vector<VenueMeta> venues(3);
  venues[0] = VenueMeta{0, "V0", -20, 30, 40'000, 70'000, 50'000, 50'000'000, true};
  venues[1] = VenueMeta{1, "V1", 15, -10, 60'000, 90'000, 70'000, 50'000'000, true};
  venues[2] = VenueMeta{2, "V2", 0, 9, 120'000, 150'000, 120'000, 50'000'000, true};

  const auto feed = DeterministicEnginePipeline::make_multi_venue_feed(40, 42, 3, 2);

  ScenarioConfig cfg{};
  cfg.scenario_name = "test-mm-multi-venue";
  cfg.seed = 42;
  cfg.risk.max_order_qty_lots = 200;
  cfg.risk.max_order_notional = Money{2'000'000LL * kMoneyScale};
  cfg.risk.max_long_position_lots = 400;
  cfg.risk.max_short_position_lots = 400;

  MarketMakerPlugin::Config mcfg{};
  mcfg.strategy.type = mmsim::StrategyType::InventoryAware;
  mcfg.strategy.base_quote_qty = 20;
  mcfg.strategy.min_quote_qty = 10;
  mcfg.strategy.min_quote_lifetime_ns = 100'000;
  mcfg.max_abs_position = 300;
  MarketMakerPlugin mm(mcfg);

  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_multi_venue(cfg, feed, mm, insts, venues);
  HFT_EXPECT_TRUE(res.all_invariants_ok());
  HFT_EXPECT_TRUE(mm.quote_decisions() > 10);
  HFT_EXPECT_TRUE(res.orders_accepted > 0);
  HFT_EXPECT_EQ(res.causality_violations, 0ULL);
  HFT_EXPECT_EQ(res.ledger_discrepancy_scaled, 0LL);

  // Verify Project 08 InventoryAware reservation price skews downward when long vs short.
  mmsim::BookSignals sig{};
  sig.valid = true;
  sig.tob = {9999, 10001, 100, 100};
  sig.mid_ticks = 10000.0;
  sig.microprice_ticks = 10000.0;
  sig.spread_ticks = 2;
  sig.realized_vol_ticks_per_sqrt_sec = 1.0;

  auto strat_impl = mmsim::make_strategy(mcfg.strategy);
  const auto q_long = strat_impl->compute_quotes(sig, mmsim::StrategyState{+150, 300, -0.2});
  const auto q_short = strat_impl->compute_quotes(sig, mmsim::StrategyState{-150, 300, -0.2});
  HFT_EXPECT_TRUE(q_long.reservation_price_ticks < q_short.reservation_price_ticks);
}

HFT_TEST(PaperArbitrageNegativeCasesAndOneLeggedLatencyRisk) {
  const InstrumentMeta inst = InstrumentMeta::make(0, "ETHUSD", 100, 1);
  std::vector<VenueMeta> venues(3);
  venues[0] = VenueMeta{0, "V0", 500, 1000, 1'000'000, 2'000'000, 1'000'000, 50'000'000, true};
  venues[1] = VenueMeta{1, "V1", 600, 1200, 2'000'000, 5'000'000, 2'000'000, 50'000'000, true};
  venues[2] = VenueMeta{2, "V2", 500, 800, 1'500'000, 3'000'000, 1'500'000, 50'000'000, true};

  PaperArbitrageAdapter::Config cfg{};
  cfg.min_net_edge_bps = 3.0;
  cfg.slippage_buffer_bps = 1.0;
  cfg.min_net_profit_quote = 0.50;
  cfg.max_stale_ns = 50'000'000;
  cfg.initial_quote_per_venue = {20'000.0, 20'000.0, 20'000.0};
  cfg.initial_base_per_venue = {100.0, 100.0, 100.0};
  PaperArbitrageAdapter arb(inst, venues, cfg);

  // 1. Negative case: +15 bps gross spread wiped out by 22 bps combined taker fees!
  const SimTimeNs t1{1'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> fee_trap = {
      {0, t1, true, {{Price{9998}, Quantity{50}, 1}}, {{Price{10000}, Quantity{50}, 1}}},
      {1, t1, true, {{Price{10015}, Quantity{50}, 1}}, {{Price{10020}, Quantity{50}, 1}}},
  };
  const auto ev1 = arb.evaluate_observed_snapshots(t1, fee_trap);
  HFT_EXPECT_EQ(ev1.size(), 1U);
  HFT_EXPECT_TRUE(ev1[0].gross_positive_net_rejected);
  HFT_EXPECT_EQ(ev1[0].status, xarb::SignalRejectionReason::InsufficientEdgeAfterFees);

  // 2. Negative case: +80 bps gross spread on stale quote (> 50ms old) rejected!
  const SimTimeNs t2{2'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> stale_snap = {
      {0, t2, true, {{Price{9998}, Quantity{50}, 1}}, {{Price{10000}, Quantity{50}, 1}}},
      {1, SimTimeNs{t2.value - 90'000'000LL}, true, {{Price{10080}, Quantity{50}, 1}},
       {{Price{10085}, Quantity{50}, 1}}},
  };
  const auto ev2 = arb.evaluate_observed_snapshots(t2, stale_snap);
  HFT_EXPECT_EQ(ev2.size(), 1U);
  HFT_EXPECT_EQ(ev2[0].status, xarb::SignalRejectionReason::StaleQuote);

  // 3. Actionable cross executes profitably when truth stays intact.
  const SimTimeNs t3{3'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> good = {
      {0, t3, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{25}, 1}}},
      {1, t3, true, {{Price{10050}, Quantity{25}, 1}}, {{Price{10055}, Quantity{50}, 1}}},
  };
  const auto ev3 = arb.evaluate_observed_snapshots(t3, good);
  HFT_EXPECT_EQ(ev3.size(), 1U);
  HFT_EXPECT_EQ(ev3[0].status, xarb::SignalRejectionReason::Actionable);
  const auto o3 = arb.execute_paper_against_delayed_truth(ev3[0], good, good, good, 100.25);
  HFT_EXPECT_EQ(o3, xarb::ExecutionOutcome::BothFilledProfitable);

  // 4. Latency decay -> sell venue drops before sell leg arrives -> OneLeggedUnhedged!
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> sell_moved = {
      {0, t3, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{25}, 1}}},
      {1, t3, true, {{Price{9980}, Quantity{25}, 1}}, {{Price{9985}, Quantity{50}, 1}}},
  };
  const auto o4 = arb.execute_paper_against_delayed_truth(ev3[0], good, good, sell_moved, 100.00);
  HFT_EXPECT_EQ(o4, xarb::ExecutionOutcome::OneLeggedUnhedged);

  const auto sum = arb.summary(100.00);
  HFT_EXPECT_TRUE(sum.invariants_ok);
  HFT_EXPECT_EQ(sum.gross_positive_net_infeasible, 2ULL);
}

int main() { return hft::test::run_all("test_multi_venue_strategies"); }
