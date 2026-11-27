#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hft/runtime/pipeline.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

namespace {

void print_usage() {
  std::cout
      << "Usage: hft_demo [options]\n"
      << "  --mode deterministic|network-lab|performance  Runtime mode (default: deterministic)\n"
      << "  --scenario NAME                               Scenario name (default: single-venue)\n"
      << "     deterministic: single-venue, itch-replay, fault-recovery, multi-venue-mm,\n"
      << "                    multi-venue-sor, paper-arb, all\n"
      << "     network-lab:   multicast-recovery, spsc-pipeline, ouch-tcp, all\n"
      << "     performance:   all\n"
      << "  --book pool|map                               Matching engine book backend (default: pool)\n"
      << "  --seed N                                      Deterministic RNG seed (default: 42)\n"
      << "  --journal PATH                                Optional OUCH binary journal output path\n"
      << "  --write-golden PATH                           Write deterministic JSON golden fixture\n"
      << "  --verify-golden PATH                          Verify output matches golden JSON fixture\n"
      << "  --json                                        Emit JSON summary\n"
      << "  --live-sockets                                Attempt live loopback UDP/TCP sockets in network-lab\n";
}

std::string read_file_str(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) return {};
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

int run_single_venue_scenario(BookBackend backend, std::uint64_t seed,
                              const std::string& journal_path, const std::string& write_golden,
                              const std::string& verify_golden, bool json_out) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "SIM_XNAS";
  venue.maker_fee_bps_x100 = -20;
  venue.taker_fee_bps_x100 = 30;
  venue.md_latency_ns = 50'000;
  venue.order_latency_ns = 80'000;
  venue.response_latency_ns = 60'000;

  ScenarioConfig cfg{};
  cfg.scenario_name = "single-venue";
  cfg.book_backend = backend;
  cfg.seed = seed;
  cfg.journal_path = journal_path;
  cfg.risk.max_order_qty_lots = 200;
  cfg.risk.max_long_position_lots = 300;
  cfg.risk.max_short_position_lots = 300;

  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  SimpleDeterministicStrategy strategy;
  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_single_venue(cfg, feed, strategy, insts, venue);

  if (json_out) {
    std::cout << res.to_json();
  } else {
    std::cout << res.to_text_summary();
    std::cout << "  trace_sample (" << res.trace_sample.size() << " entries):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(8, res.trace_sample.size()); ++i) {
      std::cout << "    - " << res.trace_sample[i] << "\n";
    }
  }

  if (!write_golden.empty()) {
    std::filesystem::create_directories(std::filesystem::path(write_golden).parent_path());
    std::ofstream ofs(write_golden);
    ofs << res.to_json();
  }
  if (!verify_golden.empty()) {
    const std::string expected = read_file_str(verify_golden);
    const std::string actual = res.to_json();
    if (expected != actual) {
      std::cerr << "ERROR: golden mismatch against " << verify_golden << "\n";
      return 2;
    }
    std::cout << "  golden_verification=PASS (" << verify_golden << ")\n";
  }

  if (!journal_path.empty()) {
    ouch::OrderManager replay_oms;
    const auto chk = OuchJournalBridge::replay_and_verify(journal_path, replay_oms);
    std::cout << "  journal_replay=" << (chk.ok ? "PASS" : "FAIL")
              << " records=" << chk.records_read
              << " orders=" << chk.orders_reconstructed << "\n";
    if (!chk.ok) return 3;
  }

  return res.all_invariants_ok() ? 0 : 1;
}

int run_itch_replay_scenario(BookBackend backend, std::uint64_t seed, bool json_out) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "S00001", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "ITCH_SIM";

  const auto raw_itch = ItchFeedAdapter::generate_synthetic_stream(1500, 1, seed);
  ItchFeedAdapter itch_adapter(0, insts);
  std::vector<Envelope> feed;
  const ErrorCode rc = itch_adapter.decode_stream(raw_itch, feed);
  if (rc != ErrorCode::Ok) {
    std::cerr << "ITCH decode failed\n";
    return 1;
  }

  // Verify canonical book matches Project 02's reference ITCH BookBuilder BBO!
  CanonicalOrderBook canon_book(1'000'000'000'000LL);
  for (const auto& env : feed) {
    canon_book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
  }
  const TopOfBook ref_tob = itch_adapter.reference_tob(1);
  const TopOfBook canon_tob = canon_book.top_of_book(SimTimeNs{100'000'000'000'000LL});
  const bool bbo_match =
      ref_tob.bid.has_value() == canon_tob.bid.has_value() &&
      ref_tob.ask.has_value() == canon_tob.ask.has_value() &&
      (!ref_tob.bid.has_value() || ref_tob.bid->price == canon_tob.bid->price) &&
      (!ref_tob.ask.has_value() || ref_tob.ask->price == canon_tob.ask->price);

  ScenarioConfig cfg{};
  cfg.scenario_name = "itch-replay";
  cfg.book_backend = backend;
  cfg.seed = seed;
  cfg.risk.max_order_qty_lots = 500;
  cfg.risk.max_order_notional = Money{10'000'000LL * kMoneyScale};
  cfg.risk.max_long_position_lots = 2000;
  cfg.risk.max_short_position_lots = 2000;

  SimpleDeterministicStrategy strat;
  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_single_venue(cfg, feed, strat, insts, venue);

  if (json_out) {
    std::cout << res.to_json();
  } else {
    std::cout << res.to_text_summary();
    std::cout << "  itch_frames=" << itch_adapter.stats().frames_decoded
              << " canonical_md=" << itch_adapter.stats().canonical_events_emitted
              << " ref_bbo_match=" << (bbo_match ? "PASS" : "FAIL") << "\n";
  }
  return (res.all_invariants_ok() && bbo_match) ? 0 : 1;
}

int run_fault_recovery_scenario(BookBackend backend, std::uint64_t seed, bool json_out) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "SIM_FAULT";

  const std::string jpath = "/tmp/hft_fault_recovery.journal";
  std::filesystem::remove(jpath);

  ScenarioConfig cfg{};
  cfg.scenario_name = "fault-recovery";
  cfg.book_backend = backend;
  cfg.seed = seed;
  cfg.journal_path = jpath;
  cfg.exec_faults.seed = seed;
  cfg.exec_faults.duplicate_every_fill = true;
  cfg.exec_faults.duplicate_report_every_n = 3;
  cfg.exec_faults.extra_ack_delay_ns = 120'000;
  cfg.exec_faults.drop_response_at_index = 4;
  cfg.exec_faults.disconnect_after_reports = 6;

  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  SimpleDeterministicStrategy strategy;
  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_single_venue(cfg, feed, strategy, insts, venue);

  ouch::OrderManager replay_oms;
  const auto jchk = OuchJournalBridge::replay_and_verify(jpath, replay_oms);

  if (json_out) {
    std::cout << res.to_json();
  } else {
    std::cout << res.to_text_summary();
    std::cout << "  fault_injection: duplicates_ignored=" << res.duplicates_ignored
              << " journal_replay=" << (jchk.ok ? "PASS" : "FAIL")
              << " journal_records=" << jchk.records_read << "\n";
  }
  return (res.all_invariants_ok() && jchk.ok && res.duplicates_ignored > 0) ? 0 : 1;
}

int run_multi_venue_mm_scenario(BookBackend backend, std::uint64_t seed, bool json_out) {
  const std::vector<InstrumentMeta> insts = {
      InstrumentMeta::make(0, "AAPL", 100, 1),
      InstrumentMeta::make(1, "MSFT", 100, 1),
  };
  std::vector<VenueMeta> venues(3);
  venues[0] = VenueMeta{0, "ARCA_SIM", -20, 30, 40'000, 70'000, 50'000, 50'000'000, true};
  venues[1] = VenueMeta{1, "BATS_INV", 15, -10, 60'000, 90'000, 70'000, 50'000'000, true};
  venues[2] = VenueMeta{2, "IEX_SIM", 0, 9, 150'000, 200'000, 150'000, 50'000'000, true};

  // Walk-forward synthetic split: in-sample seed vs out-of-sample seed (explicitly synthetic!).
  const auto feed_is = DeterministicEnginePipeline::make_multi_venue_feed(60, seed, 3, 2);
  const auto feed_oos = DeterministicEnginePipeline::make_multi_venue_feed(60, seed + 1000, 3, 2);

  auto run_one = [&](const std::string& label, const std::vector<Envelope>& feed,
                     std::uint64_t s) {
    ScenarioConfig cfg{};
    cfg.scenario_name = label;
    cfg.book_backend = backend;
    cfg.seed = s;
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
    return pipe.run_multi_venue(cfg, feed, mm, insts, venues);
  };

  const auto r_is = run_one("multi-venue-mm-synthetic-in-sample", feed_is, seed);
  const auto r_oos = run_one("multi-venue-mm-synthetic-out-of-sample", feed_oos, seed + 1000);

  if (json_out) {
    std::cout << r_oos.to_json();
  } else {
    std::cout << "[SYNTHETIC EXPERIMENT — NOT LIVE TRADING PROFITABILITY]\n";
    std::cout << r_is.to_text_summary();
    std::cout << r_oos.to_text_summary();
  }
  return (r_is.all_invariants_ok() && r_oos.all_invariants_ok()) ? 0 : 1;
}

int run_multi_venue_sor_scenario(BookBackend backend, std::uint64_t seed, bool json_out) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  std::vector<VenueMeta> venues(3);
  venues[0] = VenueMeta{0, "ARCA_SIM", -20, 30, 40'000, 80'000, 50'000, 50'000'000, true};
  venues[1] = VenueMeta{1, "BATS_INV", 20, -15, 55'000, 95'000, 65'000, 50'000'000, true};
  venues[2] = VenueMeta{2, "IEX_SIM", 0, 9, 180'000, 220'000, 180'000, 50'000'000, true};

  const auto feed = DeterministicEnginePipeline::make_multi_venue_feed(50, seed, 3, 1);

  ScenarioConfig cfg{};
  cfg.scenario_name = "multi-venue-sor";
  cfg.book_backend = backend;
  cfg.seed = seed;
  cfg.use_sor = true;
  cfg.sor_policy = sor::RoutingPolicy::SmartCostAware;
  cfg.risk.max_order_qty_lots = 200;
  cfg.risk.max_long_position_lots = 400;
  cfg.risk.max_short_position_lots = 400;

  SimpleDeterministicStrategy::Config scfg{};
  scfg.quote_qty = Quantity{40};
  scfg.inject_oversize_test_order = false;
  SimpleDeterministicStrategy strat(scfg);

  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_multi_venue(cfg, feed, strat, insts, venues);
  if (json_out) {
    std::cout << res.to_json();
  } else {
    std::cout << res.to_text_summary();
  }
  return res.all_invariants_ok() ? 0 : 1;
}

int run_paper_arb_scenario(bool json_out) {
  const InstrumentMeta inst = InstrumentMeta::make(0, "ETHUSD", 100, 1);
  std::vector<VenueMeta> venues(3);
  venues[0] = VenueMeta{0, "BINANCE_SIM", 500, 1000, 1'000'000, 2'000'000, 1'000'000, 50'000'000, true};
  venues[1] = VenueMeta{1, "COINBASE_SIM", 600, 1200, 2'000'000, 5'000'000, 2'000'000, 50'000'000, true};
  venues[2] = VenueMeta{2, "OKX_SIM", 500, 800, 1'500'000, 3'000'000, 1'500'000, 50'000'000, true};

  PaperArbitrageAdapter::Config acfg{};
  acfg.min_net_edge_bps = 3.0;
  acfg.slippage_buffer_bps = 1.0;
  acfg.min_net_profit_quote = 0.50;
  acfg.max_stale_ns = 50'000'000;
  acfg.initial_quote_per_venue = {25'000.0, 25'000.0, 25'000.0};
  acfg.initial_base_per_venue = {100.0, 100.0, 100.0};
  PaperArbitrageAdapter arb(inst, venues, acfg);

  // Case 1 (Negative): Positive gross spread (+15 bps) destroyed by combined taker fees (10 + 12 = 22 bps)!
  const SimTimeNs t1{1'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> snap_fee_trap = {
      {0, t1, true, {{Price{9998}, Quantity{50}, 1}}, {{Price{10000}, Quantity{50}, 1}}},
      {1, t1, true, {{Price{10015}, Quantity{50}, 1}}, {{Price{10020}, Quantity{50}, 1}}},
      {2, t1, true, {{Price{9999}, Quantity{50}, 1}}, {{Price{10002}, Quantity{50}, 1}}},
  };
  const auto eval1 = arb.evaluate_observed_snapshots(t1, snap_fee_trap);

  // Case 2 (Negative): Large gross spread (+80 bps) but sell venue quote is STALE (> 50ms old)!
  const SimTimeNs t2{2'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> snap_stale = {
      {0, t2, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{50}, 1}}},
      {1, SimTimeNs{t2.value - 80'000'000LL}, true, {{Price{10080}, Quantity{50}, 1}},
       {{Price{10085}, Quantity{50}, 1}}},
      {2, t2, true, {{Price{9998}, Quantity{50}, 1}}, {{Price{10002}, Quantity{50}, 1}}},
  };
  const auto eval2 = arb.evaluate_observed_snapshots(t2, snap_stale);

  // Case 3 (Positive executable): Fresh wide dislocation (+50 bps), sufficient depth & funding.
  const SimTimeNs t3{3'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> snap_good = {
      {0, t3, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{30}, 1}}},
      {1, t3, true, {{Price{10050}, Quantity{30}, 1}}, {{Price{10055}, Quantity{50}, 1}}},
      {2, t3, true, {{Price{9998}, Quantity{50}, 1}}, {{Price{10002}, Quantity{50}, 1}}},
  };
  const auto eval3 = arb.evaluate_observed_snapshots(t3, snap_good);
  for (const auto& ev : eval3) {
    if (ev.status == xarb::SignalRejectionReason::Actionable) {
      (void)arb.execute_paper_against_delayed_truth(ev, snap_good, snap_good, snap_good, 100.25);
    }
  }

  // Case 4 (One-legged latency risk): Actionable at detect time, but sell venue book drops
  // before sell leg arrives -> buy fills, sell misses (Limit-IOC) -> one-legged unhedged exposure!
  const SimTimeNs t4{4'000'000'000LL};
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> snap_race_obs = {
      {0, t4, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{25}, 1}}},
      {2, t4, true, {{Price{10045}, Quantity{25}, 1}}, {{Price{10050}, Quantity{50}, 1}}},
  };
  std::vector<PaperArbitrageAdapter::VenueSnapshotInput> snap_race_sell_moved = {
      {0, t4, true, {{Price{9995}, Quantity{50}, 1}}, {{Price{10000}, Quantity{25}, 1}}},
      {2, t4, true, {{Price{9990}, Quantity{25}, 1}}, {{Price{9995}, Quantity{50}, 1}}},
  };
  const auto eval4 = arb.evaluate_observed_snapshots(t4, snap_race_obs);
  for (const auto& ev : eval4) {
    if (ev.status == xarb::SignalRejectionReason::Actionable) {
      (void)arb.execute_paper_against_delayed_truth(ev, snap_race_obs, snap_race_obs,
                                                    snap_race_sell_moved, 100.00);
    }
  }

  const auto sum = arb.summary(100.00);
  if (json_out) {
    std::cout << "{\n"
              << "  \"scenario\": \"paper-arb\",\n"
              << "  \"paper_only\": true,\n"
              << "  \"gross_crosses\": " << sum.gross_crosses << ",\n"
              << "  \"actionable_signals\": " << sum.actionable_signals << ",\n"
              << "  \"gross_positive_net_infeasible\": " << sum.gross_positive_net_infeasible << ",\n"
              << "  \"both_filled_profitable\": " << sum.paper_stats.both_filled_profitable << ",\n"
              << "  \"one_legged_unhedged\": " << sum.paper_stats.one_legged_unhedged << ",\n"
              << "  \"invariants_ok\": " << (sum.invariants_ok ? "true" : "false") << "\n"
              << "}\n";
  } else {
    std::cout << "Scenario: paper-arb [PAPER-ONLY SYNTHETIC SIMULATION]\n"
              << "  scans=" << sum.scans
              << " gross_crosses=" << sum.gross_crosses
              << " actionable=" << sum.actionable_signals
              << " gross_pos_net_rejected=" << sum.gross_positive_net_infeasible
              << " (fees=" << sum.rejected_fees << ", stale=" << sum.rejected_stale
              << ", depth=" << sum.rejected_depth << ", funding=" << sum.rejected_funding << ")\n"
              << "  paper_trades=" << sum.paper_stats.trades_completed
              << " both_filled_profitable=" << sum.paper_stats.both_filled_profitable
              << " one_legged_unhedged=" << sum.paper_stats.one_legged_unhedged
              << " fees=$" << std::fixed << std::setprecision(4) << sum.paper_stats.total_fees_quote
              << " net_sim_pnl=$" << sum.paper_stats.net_simulated_pnl_quote
              << " invariants=" << (sum.invariants_ok ? "PASS" : "FAIL") << "\n";
  }
  (void)eval1;
  (void)eval2;
  return (sum.invariants_ok && sum.gross_positive_net_infeasible >= 2 &&
          sum.paper_stats.both_filled_profitable >= 1 &&
          sum.paper_stats.one_legged_unhedged >= 1)
             ? 0
             : 1;
}

int run_multicast_recovery_scenario(bool live_sockets) {
  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);

  MulticastFeedAdapter::Config cfg{};
  cfg.venue = 0;
  cfg.channel_id = 1;
  cfg.retransmit_history_packets = 64;
  cfg.checkpoint_every_packets = 4;
  cfg.max_gap_before_snapshot = 4;
  cfg.fault_config.seed = 42;
  cfg.fault_config.drop_prob = 0.15;
  cfg.fault_config.duplicate_prob = 0.10;
  cfg.fault_config.corrupt_prob = 0.08;
  cfg.fault_config.reorder_prob = 0.15;
  cfg.fault_config.max_reorder_delay = 2;

  MulticastFeedAdapter mdf_adapter(cfg);
  std::uint64_t now_ns = 1'000'000ULL;
  const std::size_t chunk = 4;
  for (std::size_t i = 0; i < feed.size(); i += chunk) {
    const std::size_t n = std::min(chunk, feed.size() - i);
    mdf_adapter.publish_batch(std::span<const Envelope>(feed.data() + i, n), now_ns);
    now_ns += 50'000ULL;
  }
  mdf_adapter.flush_and_recover(now_ns);
  const auto sum = mdf_adapter.summary();

  std::cout << "Scenario: multicast-recovery [MDF1/MDR1 protocol + FaultInjector]\n"
            << "  packets_published=" << sum.packets_published
            << " wire_delivered=" << sum.packets_delivered_wire
            << " gaps=" << sum.gaps_detected
            << " retransmit_recovered=" << sum.retransmit_packets_recovered
            << " snapshots=" << sum.snapshots_applied
            << " crc_errors=" << sum.checksum_errors
            << " dups=" << sum.duplicate_packets
            << " ooo=" << sum.out_of_order_buffered << "\n"
            << "  pub_hash=" << std::hex << sum.publisher_state_hash
            << " sub_hash=" << sum.subscriber_state_hash << std::dec
            << " converged=" << (sum.state_converged ? "YES" : "NO")
            << " synced=" << (sum.synced ? "YES" : "NO") << "\n";

  if (live_sockets) {
    mdf::FaultConfig live_fc{};
    live_fc.seed = 77;
    live_fc.drop_prob = 0.12;
    live_fc.duplicate_prob = 0.08;
    const auto live = MulticastFeedAdapter::run_live_udp_multicast_demo(feed, live_fc, 31041);
    std::cout << "  live_udp_multicast: started=" << (live.started ? "YES" : "NO")
              << " converged=" << (live.converged ? "YES" : "NO")
              << " gaps=" << live.gaps_detected
              << " recovered=" << live.retransmit_recovered;
    if (!live.error.empty()) std::cout << " note=" << live.error;
    std::cout << "\n";
    if (!live.started || !live.converged) return 1;
  }

  return (sum.state_converged && sum.synced) ? 0 : 1;
}

int run_spsc_pipeline_scenario() {
  const auto feed = BacktesterFeedAdapter::generate_l3_envelopes(0, 2000, 1, 42, 10'000);

  // Reference direct hash
  CanonicalOrderBook direct_book;
  for (const auto& env : feed) {
    direct_book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
  }
  const std::uint64_t direct_hash = direct_book.state_hash();

  // Two-thread SPSC pipeline (Backpressure policy -> zero loss, identical book hash)
  SpscTransportAdapter q(256, QueueOverflowPolicy::Backpressure, 500'000);
  std::atomic<bool> producer_done{false};
  std::uint64_t spsc_hash = 0;
  std::uint64_t consumed = 0;

  std::thread consumer([&]() {
    CanonicalOrderBook book;
    Envelope env{};
    while (true) {
      if (q.try_pop(env)) {
        book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
        ++consumed;
      } else if (producer_done.load(std::memory_order_acquire)) {
        while (q.try_pop(env)) {
          book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
          ++consumed;
        }
        break;
      } else {
        lfm::cpu_relax();
      }
    }
    spsc_hash = book.state_hash();
  });

  for (const auto& env : feed) {
    (void)q.push(env);
  }
  producer_done.store(true, std::memory_order_release);
  consumer.join();

  const auto st = q.stats();
  std::cout << "Scenario: spsc-pipeline [Project 05 lfm::SpscQueue]\n"
            << "  pushed=" << st.pushed << " popped=" << st.popped
            << " backpressure_spins=" << st.backpressure_spins
            << " dropped=" << st.dropped_for_resync
            << " direct_hash=" << std::hex << direct_hash
            << " spsc_hash=" << spsc_hash << std::dec
            << " match=" << (direct_hash == spsc_hash ? "PASS" : "FAIL") << "\n";
  return (direct_hash == spsc_hash && consumed == feed.size()) ? 0 : 1;
}

int run_ouch_tcp_scenario() {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  const std::string jpath = "/tmp/hft_netlab_ouch.journal";
  std::filesystem::remove(jpath);
  const auto res = run_ouch_loopback_session(insts, jpath, true);
  std::cout << "Scenario: ouch-tcp [Project 03 Gateway <-> ExchangeSim(Project 01 PoolBook)]\n"
            << "  started=" << (res.started ? "YES" : "NO")
            << " connected=" << (res.connected ? "YES" : "NO")
            << " reconnected=" << (res.reconnected ? "YES" : "NO")
            << " submitted=" << res.orders_submitted
            << " accepted=" << res.accepted
            << " filled=" << res.filled
            << " canceled=" << res.canceled
            << " invariants=" << (res.invariant_check.empty() ? "PASS" : res.invariant_check);
  if (!res.error.empty()) std::cout << " error=" << res.error;
  std::cout << "\n";
  return (res.started && res.connected && res.reconnected && res.filled >= 1 &&
          res.invariant_check.empty())
             ? 0
             : 1;
}

int run_perf_scenario() {
  const auto pr = run_performance_suite(400, 3, 10);
  std::cout << "Scenario: performance [Project 09 lpl harness — wall-clock steady timing]\n"
            << "  CPU=" << pr.env.cpu_model << " (" << pr.env.architecture << ") "
            << "compiler=" << pr.env.compiler_id << " " << pr.env.compiler_version
            << " build=" << pr.env.build_type << "\n"
            << PerfLabAdapter::format_table(pr.stage_benchmarks) << "\n"
            << PerfLabAdapter::format_table(pr.e2e_benchmarks) << "\n"
            << std::fixed << std::setprecision(1)
            << "  tick-to-decision (ns):    p50=" << pr.tick_to_decision_ns.p50
            << " p95=" << pr.tick_to_decision_ns.p95
            << " p99=" << pr.tick_to_decision_ns.p99
            << " p99.9=" << pr.tick_to_decision_ns.p999
            << " max=" << pr.tick_to_decision_ns.max << "\n"
            << "  order-submit-to-ack (ns): p50=" << pr.order_submit_to_ack_ns.p50
            << " p95=" << pr.order_submit_to_ack_ns.p95
            << " p99=" << pr.order_submit_to_ack_ns.p99
            << " p99.9=" << pr.order_submit_to_ack_ns.p999
            << " max=" << pr.order_submit_to_ack_ns.max << "\n"
            << "  order-submit-to-fill(ns): p50=" << pr.order_submit_to_fill_ns.p50
            << " p95=" << pr.order_submit_to_fill_ns.p95
            << " p99=" << pr.order_submit_to_fill_ns.p99
            << " p99.9=" << pr.order_submit_to_fill_ns.p999
            << " max=" << pr.order_submit_to_fill_ns.max << "\n"
            << "  pool_vs_map_checksum=" << (pr.pool_vs_map_checksum_match ? "MATCH" : "MISMATCH")
            << " direct_vs_spsc_checksum="
            << (pr.direct_checksum == pr.spsc_checksum ? "MATCH" : "MISMATCH") << "\n";
  return (pr.pool_vs_map_checksum_match && pr.direct_checksum == pr.spsc_checksum) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "deterministic";
  std::string scenario = "single-venue";
  std::string book = "pool";
  std::uint64_t seed = 42;
  std::string journal_path;
  std::string write_golden;
  std::string verify_golden;
  bool json_out = false;
  bool live_sockets = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else if (arg == "--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (arg == "--scenario" && i + 1 < argc) {
      scenario = argv[++i];
    } else if (arg == "--book" && i + 1 < argc) {
      book = argv[++i];
    } else if (arg == "--seed" && i + 1 < argc) {
      seed = std::stoull(argv[++i]);
    } else if (arg == "--journal" && i + 1 < argc) {
      journal_path = argv[++i];
    } else if (arg == "--write-golden" && i + 1 < argc) {
      write_golden = argv[++i];
    } else if (arg == "--verify-golden" && i + 1 < argc) {
      verify_golden = argv[++i];
    } else if (arg == "--json") {
      json_out = true;
    } else if (arg == "--live-sockets") {
      live_sockets = true;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage();
      return 1;
    }
  }

  const BookBackend backend = (book == "map") ? BookBackend::Map : BookBackend::Pool;

  if (mode == "deterministic") {
    if (scenario == "single-venue") {
      return run_single_venue_scenario(backend, seed, journal_path, write_golden, verify_golden,
                                       json_out);
    }
    if (scenario == "itch-replay") return run_itch_replay_scenario(backend, seed, json_out);
    if (scenario == "fault-recovery") return run_fault_recovery_scenario(backend, seed, json_out);
    if (scenario == "multi-venue-mm") return run_multi_venue_mm_scenario(backend, seed, json_out);
    if (scenario == "multi-venue-sor") return run_multi_venue_sor_scenario(backend, seed, json_out);
    if (scenario == "paper-arb") return run_paper_arb_scenario(json_out);
    if (scenario == "all") {
      int rc = 0;
      rc |= run_single_venue_scenario(backend, seed, journal_path, "", "", json_out);
      rc |= run_itch_replay_scenario(backend, seed, json_out);
      rc |= run_fault_recovery_scenario(backend, seed, json_out);
      rc |= run_multi_venue_mm_scenario(backend, seed, json_out);
      rc |= run_multi_venue_sor_scenario(backend, seed, json_out);
      rc |= run_paper_arb_scenario(json_out);
      return rc;
    }
  } else if (mode == "network-lab") {
    if (scenario == "multicast-recovery") return run_multicast_recovery_scenario(live_sockets);
    if (scenario == "spsc-pipeline") return run_spsc_pipeline_scenario();
    if (scenario == "ouch-tcp") return run_ouch_tcp_scenario();
    if (scenario == "all") {
      int rc = 0;
      rc |= run_multicast_recovery_scenario(live_sockets);
      rc |= run_spsc_pipeline_scenario();
      if (live_sockets) rc |= run_ouch_tcp_scenario();
      return rc;
    }
  } else if (mode == "performance") {
    return run_perf_scenario();
  }

  std::cerr << "Unsupported mode/scenario combination: " << mode << " / " << scenario << "\n";
  return 1;
}
