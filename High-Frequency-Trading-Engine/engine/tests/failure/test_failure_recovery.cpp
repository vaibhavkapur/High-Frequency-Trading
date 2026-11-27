#include <filesystem>

#include "hft/runtime/pipeline.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

HFT_TEST(DuplicateFillsAndReconnectExposureConservation) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  AccountingLedger ledger(insts, Money{1'000'000LL * kMoneyScale});
  OuchProtocolAdapter proto(insts);
  OuchJournalBridge journal(proto);
  const std::string jpath = "/tmp/hft_test_recovery.journal";
  std::filesystem::remove(jpath);
  HFT_EXPECT_TRUE(journal.open(jpath, true));

  OmsReconciler oms(ledger, &journal);
  RiskConfig rcfg{};
  rcfg.max_order_qty_lots = 100;
  rcfg.max_long_position_lots = 120;
  rcfg.max_short_position_lots = 120;
  PreTradeRiskGate risk(rcfg, insts);

  TopOfBook tob{};
  tob.bid = LevelView{Price{9999}, Quantity{100}, 1};
  tob.ask = LevelView{Price{10001}, Quantity{100}, 1};

  // Submit Order 1 (Buy 80 @ 10000). Immediately counted in worst-case exposure while PendingNew!
  OrderIntent o1{};
  o1.action = OrderAction::New;
  o1.client_order_id = 1;
  o1.token = ClientToken::from_uint64("REC", 1);
  o1.side = Side::Buy;
  o1.order_type = OrderType::Limit;
  o1.tif = TimeInForce::GTC;
  o1.limit_price = Price{10000};
  o1.qty = Quantity{80};

  HFT_EXPECT_TRUE(risk.check_order(0, 0, o1, tob, true, oms.exposure(0)).approved);
  HFT_EXPECT_TRUE(oms.on_order_sent(0, 0, o1, SimTimeNs{1'000}));
  HFT_EXPECT_EQ(oms.exposure(0).pending_buy_lots, 80ULL);

  // Disconnect BEFORE ack arrives -> Order 1 becomes Uncertain and MUST STAY in worst-case exposure!
  HFT_EXPECT_EQ(oms.on_session_disconnect(0, SimTimeNs{2'000}), 1U);
  HFT_EXPECT_EQ(oms.uncertain_orders().size(), 1U);
  HFT_EXPECT_EQ(oms.exposure(0).pending_buy_lots, 80ULL);

  // Attempting to submit Buy 50 while Order 1 (80) is Uncertain -> 80 + 50 = 130 > 120 -> REJECTED!
  OrderIntent o2 = o1;
  o2.client_order_id = 2;
  o2.token = ClientToken::from_uint64("REC", 2);
  o2.qty = Quantity{50};
  const auto rd_over = risk.check_order(0, 0, o2, tob, true, oms.exposure(0));
  HFT_EXPECT_TRUE(!rd_over.approved);
  HFT_EXPECT_EQ(rd_over.reason, RejectReason::RiskMaxPosition);

  // Reconnect & reconcile Order 1 via missed Accepted + Partial Fill (30) + Duplicate Fill (30).
  ExecutionReport ack{};
  ack.type = ExecReportType::Accepted;
  ack.client_order_id = 1;
  ack.token = o1.token;
  ack.exchange_order_id = 5001;
  ack.side = Side::Buy;
  ack.exec_price = Price{10000};
  ack.requested_qty = Quantity{80};
  ack.leaves_qty = Quantity{80};
  ack.exchange_time_ns = SimTimeNs{1'500};

  ExecutionReport fill{};
  fill.type = ExecReportType::PartiallyFilled;
  fill.client_order_id = 1;
  fill.token = o1.token;
  fill.exchange_order_id = 5001;
  fill.exec_id = 777;
  fill.side = Side::Buy;
  fill.exec_price = Price{10000};
  fill.requested_qty = Quantity{80};
  fill.fill_qty = Quantity{30};
  fill.cum_filled_qty = Quantity{30};
  fill.leaves_qty = Quantity{50};
  fill.fee_scaled = Money{90};
  fill.exchange_time_ns = SimTimeNs{1'800};

  // Deliver ack, fill, AND duplicate of the same fill (same exec_id=777)!
  const std::vector<std::pair<InstrumentId, ExecutionReport>> missed = {
      {0, ack}, {0, fill}, {0, fill}};
  HFT_EXPECT_EQ(oms.reconcile_on_reconnect(0, missed, SimTimeNs{3'000}), 2U);
  HFT_EXPECT_EQ(oms.uncertain_orders().size(), 0U);
  HFT_EXPECT_EQ(ledger.net_position(0), 30LL);  // NOT 60!
  HFT_EXPECT_EQ(oms.summary().duplicate_reports_ignored, 1ULL);
  HFT_EXPECT_EQ(oms.exposure(0).pending_buy_lots, 50ULL);

  // Cancel remaining 50 -> lost cancel response must NOT drop exposure!
  OrderIntent cxl{};
  cxl.action = OrderAction::Cancel;
  cxl.client_order_id = 3;
  cxl.token = ClientToken::from_uint64("CXL", 3);
  cxl.orig_token = o1.token;
  cxl.side = Side::Buy;
  HFT_EXPECT_TRUE(oms.on_order_sent(0, 0, cxl, SimTimeNs{4'000}));
  // Still 50 pending until exchange confirms!
  HFT_EXPECT_EQ(oms.exposure(0).pending_buy_lots, 50ULL);

  // Cancel/Fill race: remaining 50 fills on exchange BEFORE cancel reaches exchange!
  ExecutionReport fill2 = fill;
  fill2.type = ExecReportType::Filled;
  fill2.exec_id = 778;
  fill2.fill_qty = Quantity{50};
  fill2.cum_filled_qty = Quantity{80};
  fill2.leaves_qty = Quantity{0};
  HFT_EXPECT_TRUE(oms.on_execution_report(0, 0, fill2, SimTimeNs{4'500}));

  ExecutionReport cxl_rej{};
  cxl_rej.type = ExecReportType::CancelRejected;
  cxl_rej.token = o1.token;
  cxl_rej.reject_reason = RejectReason::UnknownOrder;
  HFT_EXPECT_TRUE(oms.on_execution_report(0, 0, cxl_rej, SimTimeNs{4'600}));

  const ManagedOrder* final_ord = oms.find(o1.token);
  HFT_EXPECT_TRUE(final_ord != nullptr);
  HFT_EXPECT_EQ(final_ord->state, ManagedOrderState::Filled);
  HFT_EXPECT_EQ(final_ord->cum_filled_qty.lots, 80ULL);
  HFT_EXPECT_EQ(final_ord->leaves_qty.lots, 0ULL);
  HFT_EXPECT_EQ(oms.check_invariants(), std::string{});

  const auto rec = ledger.reconcile();
  HFT_EXPECT_TRUE(rec.ok);
  HFT_EXPECT_EQ(rec.discrepancy_scaled, 0LL);

  journal.flush();
  journal.close();
  ouch::OrderManager replayed_oms;
  const auto jchk = OuchJournalBridge::replay_and_verify(jpath, replayed_oms);
  HFT_EXPECT_TRUE(jchk.ok);
  HFT_EXPECT_EQ(jchk.orders_reconstructed, 1ULL);
}

HFT_TEST(RiskGateNegativeCasesAndKillSwitch) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  RiskConfig rcfg{};
  rcfg.max_order_qty_lots = 100;
  rcfg.max_order_notional = Money{15'000LL * kMoneyScale};  // $15,000
  rcfg.max_long_position_lots = 200;
  rcfg.max_short_position_lots = 200;
  rcfg.max_price_band_bps = 200;  // 2%
  PreTradeRiskGate risk(rcfg, insts);

  TopOfBook tob{};
  tob.bid = LevelView{Price{9999}, Quantity{100}, 1};
  tob.ask = LevelView{Price{10001}, Quantity{100}, 1};
  ExposureSnapshot exp{};

  auto make_ord = [](OrderId id, std::int64_t px, std::uint64_t qty) {
    OrderIntent o{};
    o.action = OrderAction::New;
    o.client_order_id = id;
    o.token = ClientToken::from_uint64("RSK", id);
    o.side = Side::Buy;
    o.order_type = OrderType::Limit;
    o.tif = TimeInForce::GTC;
    o.limit_price = Price{px};
    o.qty = Quantity{qty};
    return o;
  };

  // 1. Stale/out-of-sync market rejected.
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(1, 10000, 10), tob, false, exp).reason,
                RejectReason::RiskStaleMarket);
  // 2. Invalid price rejected.
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(2, 0, 10), tob, true, exp).reason,
                RejectReason::InvalidPrice);
  // 3. Price band (> 2% away from 10000) rejected.
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(3, 10500, 10), tob, true, exp).reason,
                RejectReason::PriceBand);
  // 4. Oversize qty (> 100) rejected.
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(4, 10000, 150), tob, true, exp).reason,
                RejectReason::RiskMaxOrderQty);
  // 5. Oversize notional (200 @ $100 = $20,000, wait use 100 @ $100 = $10k vs $15k; use 90 @ $200?
  //    Let's test with a higher price tob: $200 = 20000 ticks, 90 * $200 = $18,000 > $15,000!)
  TopOfBook hi_tob{};
  hi_tob.bid = LevelView{Price{19999}, Quantity{100}, 1};
  hi_tob.ask = LevelView{Price{20001}, Quantity{100}, 1};
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(5, 20000, 90), hi_tob, true, exp).reason,
                RejectReason::RiskMaxNotional);
  // 6. Venue outage rejected.
  risk.set_venue_healthy(0, false);
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(6, 10000, 10), tob, true, exp).reason,
                RejectReason::RiskVenueDown);
  risk.set_venue_healthy(0, true);
  // 7. Duplicate token rejected after first approved.
  const auto o7 = make_ord(7, 10000, 10);
  HFT_EXPECT_TRUE(risk.check_order(0, 0, o7, tob, true, exp).approved);
  HFT_EXPECT_EQ(risk.check_order(0, 0, o7, tob, true, exp).reason, RejectReason::DuplicateToken);
  // 8. Kill switch blocks new orders, still permits cancels!
  risk.set_kill_switch(true);
  HFT_EXPECT_EQ(risk.check_order(0, 0, make_ord(8, 10000, 10), tob, true, exp).reason,
                RejectReason::RiskKillSwitch);
  OrderIntent cxl{};
  cxl.action = OrderAction::Cancel;
  cxl.orig_token = o7.token;
  HFT_EXPECT_TRUE(risk.check_order(0, 0, cxl, tob, false, exp).approved);
}

HFT_TEST(AccountingLedgerExactConservationThroughPositionFlips) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  AccountingLedger ledger(insts, Money{500'000LL * kMoneyScale});

  auto add_fill = [&](ExecId id, Side s, std::int64_t px, std::uint64_t qty, std::int64_t fee) {
    ExecutionReport r{};
    r.type = ExecReportType::Filled;
    r.exec_id = id;
    r.side = s;
    r.exec_price = Price{px};
    r.fill_qty = Quantity{qty};
    r.fee_scaled = Money{fee};
    return ledger.on_fill(0, 0, r);
  };

  // Long 10 @ 100.00, add 20 @ 100.05 (odd avg cost!), sell 15 @ 100.10 (partial close),
  // sell 25 @ 99.90 (flips from +15 long to -10 short!), buy 10 @ 99.80 (covers flat!).
  HFT_EXPECT_TRUE(add_fill(1, Side::Buy, 10000, 10, 30));
  HFT_EXPECT_TRUE(add_fill(2, Side::Buy, 10005, 20, -20));
  HFT_EXPECT_TRUE(add_fill(3, Side::Sell, 10010, 15, 45));
  ledger.update_mark(0, Price{10020});
  auto r_mid = ledger.reconcile();
  HFT_EXPECT_TRUE(r_mid.ok);
  HFT_EXPECT_EQ(r_mid.discrepancy_scaled, 0LL);

  HFT_EXPECT_TRUE(add_fill(4, Side::Sell, 9990, 25, 75));
  HFT_EXPECT_EQ(ledger.net_position(0), -10LL);
  ledger.update_mark(0, Price{9985});
  auto r_short = ledger.reconcile();
  HFT_EXPECT_TRUE(r_short.ok);
  HFT_EXPECT_EQ(r_short.discrepancy_scaled, 0LL);

  HFT_EXPECT_TRUE(add_fill(5, Side::Buy, 9980, 10, 30));
  HFT_EXPECT_EQ(ledger.net_position(0), 0LL);
  auto r_flat = ledger.reconcile();
  HFT_EXPECT_TRUE(r_flat.ok);
  HFT_EXPECT_EQ(r_flat.discrepancy_scaled, 0LL);
  HFT_EXPECT_EQ(r_flat.unrealized_pnl.scaled, 0LL);
  // Exact total net P&L check:
  // Bought: 10*100.00 + 20*100.05 + 10*99.80 = 1000 + 2001 + 998 = $3999.00
  // Sold:   15*100.10 + 25*99.90            = 1501.50 + 2497.50 = $3999.00 -> gross PnL = $0.00!
  // Total fees: 30 - 20 + 45 + 75 + 30 = +160 ($0.0160)
  HFT_EXPECT_EQ(r_flat.realized_pnl.scaled, 0LL);
  HFT_EXPECT_EQ(r_flat.total_fees.scaled, 160LL);
  HFT_EXPECT_EQ(r_flat.net_equity_cash_plus_inv.scaled, 500'000LL * kMoneyScale - 160LL);
}

HFT_TEST(EndToEndSeededFaultMatrixConvergesWithZeroViolations) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;

  ScenarioConfig cfg{};
  cfg.scenario_name = "fault-matrix";
  cfg.seed = 123;
  cfg.exec_faults.duplicate_every_fill = true;
  cfg.exec_faults.duplicate_report_every_n = 2;
  cfg.exec_faults.extra_ack_delay_ns = 100'000;
  cfg.exec_faults.drop_response_at_index = 3;
  cfg.exec_faults.disconnect_after_reports = 5;

  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  SimpleDeterministicStrategy strat;
  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_single_venue(cfg, feed, strat, insts, venue);

  HFT_EXPECT_TRUE(res.all_invariants_ok());
  HFT_EXPECT_TRUE(res.duplicates_ignored > 0);
  HFT_EXPECT_EQ(res.ledger_discrepancy_scaled, 0LL);
}

int main() { return hft::test::run_all("test_failure_recovery"); }
