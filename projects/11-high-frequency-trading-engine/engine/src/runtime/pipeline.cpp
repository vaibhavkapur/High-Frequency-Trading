#include "hft/runtime/pipeline.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <thread>

namespace hft::runtime {

using namespace contracts::v1;

namespace {

std::string hex_u64(std::uint64_t v) {
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << v;
  return oss.str();
}

}  // namespace

std::string DeterministicRunResult::to_json() const {
  std::ostringstream oss;
  oss << "{\n"
      << "  \"scenario\": \"" << scenario_name << "\",\n"
      << "  \"book_backend\": \"" << book_backend_name << "\",\n"
      << "  \"seed\": " << seed << ",\n"
      << "  \"events_scheduled\": " << events_scheduled << ",\n"
      << "  \"md_events_observed\": " << md_events_observed << ",\n"
      << "  \"intents_emitted\": " << intents_emitted << ",\n"
      << "  \"risk_approved\": " << risk_approved << ",\n"
      << "  \"risk_rejected\": " << risk_rejected << ",\n"
      << "  \"orders_accepted\": " << orders_accepted << ",\n"
      << "  \"fills_executed\": " << fills_executed << ",\n"
      << "  \"orders_canceled\": " << orders_canceled << ",\n"
      << "  \"orders_rejected\": " << orders_rejected << ",\n"
      << "  \"duplicates_ignored\": " << duplicates_ignored << ",\n"
      << "  \"causality_violations\": " << causality_violations << ",\n"
      << "  \"replay_checksum\": \"" << hex_u64(replay_checksum) << "\",\n"
      << "  \"final_book_hash\": \"" << hex_u64(final_book_hash) << "\",\n"
      << "  \"final_position_lots\": " << final_position_lots << ",\n"
      << "  \"cash_scaled\": " << cash_scaled.scaled << ",\n"
      << "  \"realized_pnl_scaled\": " << realized_pnl_scaled.scaled << ",\n"
      << "  \"unrealized_pnl_scaled\": " << unrealized_pnl_scaled.scaled << ",\n"
      << "  \"fees_scaled\": " << fees_scaled.scaled << ",\n"
      << "  \"net_equity_scaled\": " << net_equity_scaled.scaled << ",\n"
      << "  \"ledger_discrepancy_scaled\": " << ledger_discrepancy_scaled << ",\n"
      << "  \"all_invariants_ok\": " << (all_invariants_ok() ? "true" : "false") << "\n"
      << "}\n";
  return oss.str();
}

std::string DeterministicRunResult::to_text_summary() const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(4);
  oss << "Scenario: " << scenario_name << " [backend=" << book_backend_name
      << ", seed=" << seed << "]\n"
      << "  events_scheduled=" << events_scheduled
      << " md_observed=" << md_events_observed
      << " intents=" << intents_emitted
      << " risk_ok=" << risk_approved
      << " risk_rej=" << risk_rejected << "\n"
      << "  accepted=" << orders_accepted
      << " fills=" << fills_executed
      << " canceled=" << orders_canceled
      << " rejected=" << orders_rejected
      << " dup_ignored=" << duplicates_ignored << "\n"
      << "  position=" << final_position_lots
      << " cash=$" << cash_scaled.to_dollars()
      << " realized=$" << realized_pnl_scaled.to_dollars()
      << " unrealized=$" << unrealized_pnl_scaled.to_dollars()
      << " fees=$" << fees_scaled.to_dollars()
      << " equity=$" << net_equity_scaled.to_dollars() << "\n"
      << "  replay_checksum=" << hex_u64(replay_checksum)
      << " final_book_hash=" << hex_u64(final_book_hash)
      << " invariants=" << (all_invariants_ok() ? "PASS" : "FAIL");
  if (!invariant_error.empty()) {
    oss << " (" << invariant_error << ")";
  }
  oss << "\n";
  return oss.str();
}

std::vector<Envelope> DeterministicEnginePipeline::make_golden_single_venue_feed(
    VenueId venue, InstrumentId instrument) {
  std::vector<Envelope> feed;
  EventSeq seq = 1;
  std::int64_t t_ns = 1'000'000'000LL;  // t = 1.0s

  auto add_ev = [&](MdUpdateKind kind, std::uint64_t ref, Side side, std::int64_t px,
                    std::uint64_t qty, std::int64_t dt_ns = 500'000LL) {
    t_ns += dt_ns;
    MarketDataEvent md{};
    md.kind = kind;
    md.channel = 1;
    md.feed_seq = seq;
    md.source_time_ns = SimTimeNs{t_ns};
    md.receive_time_ns = SimTimeNs{t_ns};
    md.feed_order_ref = FeedOrderRef{ref};
    md.side = side;
    md.price = Price{px};
    md.qty = Quantity{qty};

    Envelope env{};
    env.contract_version = kContractVersion;
    env.sequence = seq++;
    env.event_time_ns = SimTimeNs{t_ns};
    env.priority = PriorityClass::MarketDataDelta;
    env.instrument = instrument;
    env.venue = venue;
    env.payload = md;
    feed.push_back(env);
  };

  // Seed initial 4-level L3 book around $100.00 (10,000 ticks at $0.01/tick).
  add_ev(MdUpdateKind::Add, 1001, Side::Buy, 9999, 60);
  add_ev(MdUpdateKind::Add, 1002, Side::Buy, 9998, 120);
  add_ev(MdUpdateKind::Add, 1003, Side::Buy, 9997, 200);
  add_ev(MdUpdateKind::Add, 2001, Side::Sell, 10001, 25);  // smaller than crossing order -> partial fill!
  add_ev(MdUpdateKind::Add, 2002, Side::Sell, 10002, 100);
  add_ev(MdUpdateKind::Add, 2003, Side::Sell, 10003, 180);

  // Dynamic L3 flow: additions, partial cancel, aggressive market sweeps (to fill passive quotes),
  // replacements, and mid-price oscillations.
  std::uint64_t next_ref = 3000;
  for (int i = 0; i < 40; ++i) {
    const std::int64_t shift = (i % 8 < 4) ? (i % 4) : (4 - (i % 4));
    const std::int64_t bid_px = 9999 + shift;
    const std::int64_t ask_px = bid_px + 2;
    const std::uint64_t b_ref = next_ref++;
    const std::uint64_t a_ref = next_ref++;
    add_ev(MdUpdateKind::Add, b_ref, Side::Buy, bid_px, 40 + static_cast<std::uint64_t>(i % 3) * 20);
    add_ev(MdUpdateKind::Add, a_ref, Side::Sell, ask_px, 35 + static_cast<std::uint64_t>(i % 4) * 15);
    if (i % 5 == 2) {
      add_ev(MdUpdateKind::Cancel, b_ref, Side::Buy, bid_px, 15);
    }
    if (i % 6 == 3) {
      // Aggressive sell sweep crossing passive resting bids on the exchange!
      add_ev(MdUpdateKind::Trade, 900'000ULL + static_cast<std::uint64_t>(i), Side::Sell,
             bid_px - 1, 25);
    }
    if (i % 7 == 4) {
      add_ev(MdUpdateKind::Delete, a_ref, Side::Sell, ask_px, 0);
    }
  }
  return feed;
}

std::vector<Envelope> DeterministicEnginePipeline::make_multi_venue_feed(
    std::size_t steps, std::uint64_t seed, std::size_t num_venues, std::size_t num_instruments) {
  std::vector<Envelope> feed;
  EventSeq seq = 1;
  std::int64_t t_ns = 2'000'000'000LL;
  std::uint64_t rng = seed ^ 0x9E3779B97F4A7C15ULL;
  auto next_u64 = [&]() {
    rng ^= rng << 13U;
    rng ^= rng >> 7U;
    rng ^= rng << 17U;
    return rng;
  };

  std::uint64_t ref = 10'000;
  for (std::size_t s = 0; s < steps; ++s) {
    for (std::size_t inst = 0; inst < num_instruments; ++inst) {
      const std::int64_t base_mid = 10'000 + static_cast<std::int64_t>(inst) * 5'000 +
                                    static_cast<std::int64_t>((s / 10) % 7) - 3;
      for (std::size_t v = 0; v < num_venues; ++v) {
        t_ns += 120'000LL;
        // Slight per-venue price dispersion to exercise smart routing & market making.
        const std::int64_t venue_skew = static_cast<std::int64_t>(v) - 1;
        const std::int64_t bid_px = base_mid - 1 + venue_skew;
        const std::int64_t ask_px = base_mid + 1 + venue_skew;
        const std::uint64_t bq = 40 + (next_u64() % 80);
        const std::uint64_t aq = 40 + (next_u64() % 80);

        auto push_ev = [&](MdUpdateKind kind, Side side, std::int64_t px, std::uint64_t qty) {
          MarketDataEvent md{};
          md.kind = kind;
          md.channel = static_cast<std::uint16_t>(v + 1);
          md.feed_seq = seq;
          md.source_time_ns = SimTimeNs{t_ns};
          md.receive_time_ns = SimTimeNs{t_ns};
          md.feed_order_ref = FeedOrderRef{ref++};
          md.side = side;
          md.price = Price{px};
          md.qty = Quantity{qty};

          Envelope env{};
          env.contract_version = kContractVersion;
          env.sequence = seq++;
          env.event_time_ns = SimTimeNs{t_ns};
          env.priority = PriorityClass::MarketDataDelta;
          env.instrument = static_cast<InstrumentId>(inst);
          env.venue = static_cast<VenueId>(v);
          env.payload = md;
          feed.push_back(env);
        };

        push_ev(MdUpdateKind::Add, Side::Buy, bid_px, bq);
        push_ev(MdUpdateKind::Add, Side::Sell, ask_px, aq);
        if (s % 4 == 2) {
          // Periodic aggressive trade sweep on venue to fill passive MM quotes.
          const Side agg_side = ((s + v) % 2 == 0) ? Side::Sell : Side::Buy;
          const std::int64_t sweep_px = (agg_side == Side::Sell) ? (bid_px - 2) : (ask_px + 2);
          push_ev(MdUpdateKind::Trade, agg_side, sweep_px, 20);
        }
      }
    }
  }
  return feed;
}

namespace {

class ObservedViewImpl final : public IObservedMarketView {
 public:
  ObservedViewImpl(const DeterministicScheduler& sched,
                   const std::vector<std::vector<CanonicalOrderBook>>& obs_books,
                   const AccountingLedger& ledger)
      : sched_(sched), obs_books_(obs_books), ledger_(ledger) {}

  [[nodiscard]] SimTimeNs now() const noexcept override { return sched_.now(); }

  [[nodiscard]] TopOfBook observed_tob(VenueId venue, InstrumentId instrument) const override {
    if (venue >= obs_books_.size() || instrument >= obs_books_[venue].size()) return TopOfBook{};
    return obs_books_[venue][instrument].top_of_book(sched_.now());
  }

  [[nodiscard]] std::vector<LevelView> observed_depth(VenueId venue, InstrumentId instrument,
                                                      Side side,
                                                      std::size_t max_levels) const override {
    if (venue >= obs_books_.size() || instrument >= obs_books_[venue].size()) return {};
    return obs_books_[venue][instrument].depth(side, max_levels);
  }

  [[nodiscard]] bool is_tradable(VenueId venue, InstrumentId instrument) const override {
    if (venue >= obs_books_.size() || instrument >= obs_books_[venue].size()) return false;
    return obs_books_[venue][instrument].is_tradable(sched_.now());
  }

  [[nodiscard]] std::int64_t net_position(InstrumentId instrument) const override {
    return ledger_.net_position(instrument);
  }

 private:
  const DeterministicScheduler& sched_;
  const std::vector<std::vector<CanonicalOrderBook>>& obs_books_;
  const AccountingLedger& ledger_;
};

}  // namespace

DeterministicRunResult DeterministicEnginePipeline::run_single_venue(
    const ScenarioConfig& cfg, const std::vector<Envelope>& feed, IStrategy& strategy,
    const std::vector<InstrumentMeta>& insts, const VenueMeta& venue) {
  return run_multi_venue(cfg, feed, strategy, insts, std::vector<VenueMeta>{venue});
}

DeterministicRunResult DeterministicEnginePipeline::run_multi_venue(
    const ScenarioConfig& cfg, const std::vector<Envelope>& feed, IStrategy& strategy,
    const std::vector<InstrumentMeta>& insts, const std::vector<VenueMeta>& venues) {
  DeterministicRunResult res{};
  res.scenario_name = cfg.scenario_name;
  res.book_backend_name = (cfg.book_backend == adapters::BookBackend::Pool) ? "pool" : "map";
  res.seed = cfg.seed;

  DeterministicScheduler sched;
  AccountingLedger ledger(insts);
  adapters::OuchProtocolAdapter ouch_proto(insts);
  std::unique_ptr<adapters::OuchJournalBridge> journal;
  if (!cfg.journal_path.empty()) {
    journal = std::make_unique<adapters::OuchJournalBridge>(ouch_proto);
    (void)journal->open(cfg.journal_path, false);
  }
  OmsReconciler oms(ledger, journal.get());
  PreTradeRiskGate risk(cfg.risk, insts);
  ExecutionFaultInjector faults(cfg.exec_faults);

  std::unique_ptr<adapters::SmartRouterAdapter> router;
  if (cfg.use_sor && venues.size() > 1) {
    router = std::make_unique<adapters::SmartRouterAdapter>(insts, venues, cfg.sor_policy);
  }

  std::vector<std::unique_ptr<adapters::MatchingEngineAdapter>> exchanges;
  std::vector<std::unique_ptr<adapters::CausalLatencyAdapter>> latencies;
  std::vector<std::vector<CanonicalOrderBook>> obs_books(venues.size());

  for (std::size_t v = 0; v < venues.size(); ++v) {
    exchanges.push_back(std::make_unique<adapters::MatchingEngineAdapter>(
        venues[v], insts, cfg.book_backend));
    latencies.push_back(std::make_unique<adapters::CausalLatencyAdapter>(
        venues[v].md_latency_ns, 0, venues[v].order_latency_ns, 0,
        venues[v].response_latency_ns, 0, cfg.seed + v));
    risk.set_venue_healthy(venues[v].id, venues[v].enabled);
    obs_books[v].reserve(insts.size());
    for (std::size_t i = 0; i < insts.size(); ++i) {
      obs_books[v].emplace_back(venues[v].stale_threshold_ns);
    }
  }

  ObservedViewImpl obs_view(sched, obs_books, ledger);
  std::uint64_t trace_hash = 14695981039346656037ULL;
  EventSeq trace_seq = 1;

  auto record_trace = [&](const Envelope& env, const std::string& note) {
    trace_hash = hash_envelope(env, trace_hash);
    if (res.trace_sample.size() < 24 && !note.empty()) {
      res.trace_sample.push_back(note);
    }
  };

  // Forward declaration via std::function for scheduling execution report delivery & order dispatch.
  std::function<void(VenueId, InstrumentId, const ExecutionReport&, SimTimeNs)> schedule_exec_delivery;
  std::function<void(VenueId, InstrumentId, const OrderIntent&, SimTimeNs)> dispatch_intent;

  schedule_exec_delivery = [&](VenueId venue_id, InstrumentId inst_id,
                               const ExecutionReport& raw_report, SimTimeNs exch_time) {
    const FaultInjectionResult fi = faults.evaluate(inst_id, raw_report);
    if (fi.drop_report) {
      // Dropped response will be recovered on reconnect reconciliation.
      return;
    }

    auto deliver_one = [&](ExecutionReport r, SimTimeNs deliver_time) {
      r.gateway_recv_time_ns = deliver_time;
      Envelope env{};
      env.contract_version = kContractVersion;
      env.sequence = trace_seq++;
      env.event_time_ns = deliver_time;
      env.priority = PriorityClass::GatewayExecDelivery;
      env.instrument = inst_id;
      env.venue = venue_id;
      env.payload = r;

      sched.schedule(deliver_time, PriorityClass::GatewayExecDelivery, env.sequence, env,
                     [&, venue_id, inst_id](const Envelope& e) {
                       const auto& rep = std::get<ExecutionReport>(e.payload);
                       // Causality invariant: exec delivery >= exchange time.
                       if (rep.gateway_recv_time_ns < rep.exchange_time_ns) {
                         ++res.causality_violations;
                       }
                       const bool applied = oms.on_execution_report(venue_id, inst_id, rep,
                                                                    sched.now());
                       record_trace(e, std::string("EXEC t=") + std::to_string(e.event_time_ns.value) +
                                           " tok=" + rep.token.to_string() + " " +
                                           to_string(rep.type) + " fill=" +
                                           std::to_string(rep.fill_qty.lots) +
                                           (applied ? "" : " [DUP_IGNORED]"));
                       if (!applied) return;

                       if (router != nullptr) {
                         std::vector<adapters::RoutedChildOrder> reroutes;
                         router->on_child_execution_report(inst_id, venue_id, rep, sched.now(),
                                                           reroutes);
                         for (const auto& rc : reroutes) {
                           dispatch_intent(rc.venue, rc.instrument, rc.intent, sched.now());
                         }
                       }

                       std::vector<OrderIntent> followups;
                       strategy.on_execution_report(obs_view, venue_id, inst_id, rep, followups);
                       for (const auto& f : followups) {
                         dispatch_intent(venue_id, inst_id, f, sched.now());
                       }
                     });
    };

    const SimTimeNs base_deliver =
        latencies[venue_id]->deliver_response(exch_time) + fi.extra_delay_ns;
    deliver_one(raw_report, base_deliver);
    if (fi.emit_duplicate) {
      deliver_one(raw_report, base_deliver + 10'000LL);
    }
    if (fi.trigger_disconnect) {
      const SimTimeNs disc_t = base_deliver + 5'000LL;
      Envelope disc_env{};
      disc_env.sequence = trace_seq++;
      disc_env.event_time_ns = disc_t;
      disc_env.priority = PriorityClass::SystemControl;
      disc_env.venue = venue_id;
      disc_env.payload = SystemEvent::make(SystemEvent::Kind::SessionDown,
                                           ErrorCode::SessionDisconnected, venue_id,
                                           "injected_gateway_disconnect");
      sched.schedule(disc_t, PriorityClass::SystemControl, disc_env.sequence, disc_env,
                     [&, venue_id](const Envelope& e) {
                       (void)oms.on_session_disconnect(venue_id, sched.now());
                       record_trace(e, "SYS SESSION_DOWN venue=" + std::to_string(venue_id));
                     });

      // Schedule reconnect & reconciliation 5ms later.
      const SimTimeNs rec_t = disc_t + 5'000'000LL;
      Envelope rec_env{};
      rec_env.sequence = trace_seq++;
      rec_env.event_time_ns = rec_t;
      rec_env.priority = PriorityClass::SystemControl;
      rec_env.venue = venue_id;
      rec_env.payload = SystemEvent::make(SystemEvent::Kind::SessionUp, ErrorCode::Ok, venue_id,
                                          "reconnected_and_reconciled");
      sched.schedule(rec_t, PriorityClass::SystemControl, rec_env.sequence, rec_env,
                     [&, venue_id](const Envelope& e) {
                       (void)oms.reconcile_on_reconnect(venue_id, faults.buffered_for_reconnect(),
                                                        sched.now());
                       faults.clear_reconnect_buffer();
                       record_trace(e, "SYS SESSION_UP_RECONCILED venue=" + std::to_string(venue_id));
                     });
    }
  };

  dispatch_intent = [&](VenueId venue_id, InstrumentId inst_id, const OrderIntent& intent,
                        SimTimeNs now_ns) {
    ++res.intents_emitted;
    const TopOfBook tob = obs_books[venue_id][inst_id].top_of_book(now_ns);
    const bool tradable = obs_books[venue_id][inst_id].is_tradable(now_ns);
    const ExposureSnapshot exp = oms.exposure(inst_id);
    const RiskDecision rd = risk.check_order(venue_id, inst_id, intent, tob, tradable, exp);

    Envelope intent_env{};
    intent_env.sequence = trace_seq++;
    intent_env.event_time_ns = now_ns;
    intent_env.priority = PriorityClass::StrategyOrderIntent;
    intent_env.instrument = inst_id;
    intent_env.venue = venue_id;
    intent_env.payload = intent;

    if (!rd.approved) {
      ExecutionReport rej{};
      rej.type = ExecReportType::Rejected;
      rej.client_order_id = intent.client_order_id;
      rej.token = intent.token;
      rej.side = intent.side;
      rej.exec_price = intent.limit_price;
      rej.requested_qty = intent.qty;
      rej.reject_reason = rd.reason;
      rej.exchange_time_ns = now_ns;
      rej.gateway_recv_time_ns = now_ns;
      Envelope rej_env{};
      rej_env.sequence = trace_seq++;
      rej_env.event_time_ns = now_ns;
      rej_env.priority = PriorityClass::GatewayExecDelivery;
      rej_env.instrument = inst_id;
      rej_env.venue = venue_id;
      rej_env.payload = rej;
      record_trace(rej_env, std::string("RISK_REJECT tok=") + intent.token.to_string() +
                                " reason=" + to_string(rd.reason));
      std::vector<OrderIntent> ignored;
      strategy.on_execution_report(obs_view, venue_id, inst_id, rej, ignored);
      return;
    }

    // If SOR is enabled for new parent orders, split via Project 07 router before venue dispatch!
    if (router != nullptr && intent.action == OrderAction::New && intent.parent_id == 0) {
      std::vector<adapters::RoutedChildOrder> children;
      const ParentOrderId pid = router->route_parent(inst_id, intent, now_ns, children);
      if (pid != 0 && !children.empty()) {
        for (const auto& rc : children) {
          dispatch_intent(rc.venue, rc.instrument, rc.intent, now_ns);
        }
        return;
      }
    }

    if (!oms.on_order_sent(venue_id, inst_id, intent, now_ns)) {
      return;
    }
    record_trace(intent_env, std::string("ORDER_SENT tok=") + intent.token.to_string() +
                                 " side=" + to_string(intent.side) + " px=" +
                                 std::to_string(intent.limit_price.ticks) + " qty=" +
                                 std::to_string(intent.qty.lots));

    const SimTimeNs arrival_ns = latencies[venue_id]->deliver_order(now_ns);
    Envelope arr_env = intent_env;
    arr_env.sequence = trace_seq++;
    arr_env.event_time_ns = arrival_ns;
    arr_env.priority = PriorityClass::ExchangeOrderArrival;

    sched.schedule(arrival_ns, PriorityClass::ExchangeOrderArrival, arr_env.sequence, arr_env,
                   [&, venue_id, inst_id, sent_time = now_ns](const Envelope& e) {
                     const auto& oi = std::get<OrderIntent>(e.payload);
                     if (sched.now() < sent_time) {
                       ++res.causality_violations;
                     }
                     std::vector<ExecutionReport> reports;
                     (void)exchanges[venue_id]->submit_order(inst_id, oi, sched.now(), reports);
                     for (const auto& r : reports) {
                       schedule_exec_delivery(venue_id, inst_id, r, sched.now());
                     }
                   });
  };

  // Schedule all source market data events on the venue truth timeline.
  for (const auto& src_env : feed) {
    if (!std::holds_alternative<MarketDataEvent>(src_env.payload)) continue;
    if (src_env.venue >= venues.size() || src_env.instrument >= insts.size()) continue;

    sched.schedule(src_env.event_time_ns, PriorityClass::MarketDataDelta, src_env.sequence, src_env,
                   [&](const Envelope& truth_env) {
                     const auto& md = std::get<MarketDataEvent>(truth_env.payload);
                     const VenueId vid = truth_env.venue;
                     const InstrumentId iid = truth_env.instrument;

                     // 1. Update authoritative exchange truth book (Project 01) and emit any passive fills.
                     std::vector<ExecutionReport> passive_fills;
                     exchanges[vid]->apply_market_event_with_fills(iid, md, sched.now(),
                                                                   passive_fills);
                     for (const auto& pf : passive_fills) {
                       schedule_exec_delivery(vid, iid, pf, sched.now());
                     }

                     // Update mark price from truth mid/last for accounting reconciliation.
                     const TopOfBook truth_tob = exchanges[vid]->top_of_book(iid);
                     if (truth_tob.valid_two_sided()) {
                       ledger.update_mark(
                           iid, Price{(truth_tob.bid->price.ticks + truth_tob.ask->price.ticks) / 2});
                     }

                     // 2. Schedule delayed market observation to strategy (strictly >= source_time_ns).
                     const SimTimeNs obs_t = latencies[vid]->deliver_md(truth_env.event_time_ns);
                     MarketDataEvent obs_md = md;
                     obs_md.receive_time_ns = obs_t;
                     Envelope obs_env = truth_env;
                     obs_env.sequence = trace_seq++;
                     obs_env.event_time_ns = obs_t;
                     obs_env.priority = PriorityClass::StrategyObservation;
                     obs_env.payload = obs_md;

                     sched.schedule(
                         obs_t, PriorityClass::StrategyObservation, obs_env.sequence, obs_env,
                         [&, vid, iid](const Envelope& e) {
                           const auto& omd = std::get<MarketDataEvent>(e.payload);
                           if (e.event_time_ns < omd.source_time_ns) {
                             ++res.causality_violations;
                           }
                           ++res.md_events_observed;
                           obs_books[vid][iid].apply(omd, sched.now());

                           if (router != nullptr) {
                             router->update_venue_depth(
                                 iid, vid, obs_books[vid][iid].depth(Side::Buy, 8),
                                 obs_books[vid][iid].depth(Side::Sell, 8), omd.source_time_ns,
                                 sched.now());
                           }

                           std::vector<OrderIntent> intents;
                           strategy.on_observed_market_data(obs_view, vid, iid, omd, intents);
                           for (const auto& oi : intents) {
                             dispatch_intent(vid, iid, oi, sched.now());
                           }
                         });
                   });
  }

  sched.run_all();

  // Final reconciliation of any remaining dropped reports if disconnect never fired.
  if (!faults.buffered_for_reconnect().empty()) {
    (void)oms.reconcile_on_reconnect(0, faults.buffered_for_reconnect(), sched.now());
    faults.clear_reconnect_buffer();
  }
  if (journal != nullptr) {
    journal->flush();
    journal->close();
  }

  res.events_scheduled = sched.processed_count();
  res.causality_violations += sched.causality_violations();
  res.risk_approved = risk.stats().approved;
  res.risk_rejected = risk.stats().rejected;
  res.orders_accepted = oms.summary().accepted;
  res.fills_executed = ledger.execution_history().size();
  res.orders_canceled = oms.summary().canceled;
  res.orders_rejected = oms.summary().rejected;
  res.duplicates_ignored =
      oms.summary().duplicate_reports_ignored + ledger.duplicate_exec_ids_ignored();
  res.replay_checksum = trace_hash;

  // Hash all venue books deterministically.
  std::uint64_t bhash = 14695981039346656037ULL;
  bool exch_ok = true;
  std::string err;
  for (std::size_t v = 0; v < exchanges.size(); ++v) {
    if (!exchanges[v]->check_invariants(&err)) {
      exch_ok = false;
    }
    for (std::size_t i = 0; i < insts.size(); ++i) {
      const std::uint64_t h = exchanges[v]->book_hash(static_cast<InstrumentId>(i));
      bhash ^= h;
      bhash *= 1099511628211ULL;
    }
  }
  res.final_book_hash = bhash;
  res.exchange_invariants_ok = exch_ok;

  const std::string oms_err = oms.check_invariants();
  res.oms_invariants_ok = oms_err.empty();
  if (!oms_err.empty() && err.empty()) err = oms_err;

  const ReconciliationReport rec = ledger.reconcile();
  res.ledger_reconciled_ok = rec.ok;
  res.ledger_discrepancy_scaled = rec.discrepancy_scaled;
  if (!rec.ok && err.empty()) err = rec.failure_reason;
  res.invariant_error = err;

  res.final_position_lots = insts.empty() ? 0 : ledger.net_position(0);
  res.cash_scaled = rec.cash_scaled;
  res.realized_pnl_scaled = rec.realized_pnl;
  res.unrealized_pnl_scaled = rec.unrealized_pnl;
  res.fees_scaled = rec.total_fees;
  res.net_equity_scaled = rec.net_equity_cash_plus_inv;
  return res;
}

// ---- Phase 6: Reproducible Wall-Clock Performance Lab Suite ----

PerfSuiteResult run_performance_suite(std::size_t events_per_sample, std::size_t repetitions,
                                      std::size_t samples_per_rep) {
  PerfSuiteResult out{};
  out.env = adapters::PerfLabAdapter::collect_environment();

  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "SIM_V0";

  // Pre-generate ITCH synthetic stream (Project 02) & canonical feed (Project 04).
  const auto itch_bytes = adapters::ItchFeedAdapter::generate_synthetic_stream(
      events_per_sample, 1, 42);
  const auto canon_feed = adapters::BacktesterFeedAdapter::generate_l3_envelopes(
      0, events_per_sample, 1, 42, 10'000);

  lpl::BenchmarkConfig bcfg{};
  bcfg.warmup_rounds = 2;
  bcfg.repetitions = repetitions;
  bcfg.samples_per_rep = samples_per_rep;
  bcfg.ops_per_sample = events_per_sample;
  bcfg.pin_core = -1;  // macOS-safe; Linux scripts can set core affinity

  // 1. Stage: ITCH 5.0 Decode -> Canonical Envelope (Project 02)
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "01_itch_decode", "project02_adapter", events_per_sample, bcfg, [&]() {
        adapters::ItchFeedAdapter adapter(0, insts);
        std::vector<Envelope> evs;
        evs.reserve(events_per_sample);
        (void)adapter.decode_stream(itch_bytes, evs, events_per_sample);
        return static_cast<std::uint64_t>(evs.size());
      }));

  // 2. Stage: Canonical Observed Book Update
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "02_book_update", "canonical_l3_book", canon_feed.size(), bcfg, [&]() {
        CanonicalOrderBook book;
        for (const auto& env : canon_feed) {
          book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
        }
        return book.state_hash();
      }));

  // 3. Stage: Strategy Compute (Project 08 FeatureEngine + InventoryAwareStrategy)
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "03_strategy_compute", "mmsim_inventory_mm", events_per_sample, bcfg, [&]() {
        mmsim::StrategyConfig scfg{};
        scfg.type = mmsim::StrategyType::InventoryAware;
        auto strat = mmsim::make_strategy(scfg);
        mmsim::BookSignals sig{};
        sig.valid = true;
        sig.tob = {9999, 10001, 100, 100};
        sig.mid_ticks = 10000.0;
        sig.microprice_ticks = 10000.2;
        sig.spread_ticks = 2;
        sig.spread_bps = 2.0;
        sig.l1_imbalance = 0.1;
        sig.topk_imbalance = 0.15;
        sig.realized_vol_ticks_per_sqrt_sec = 0.8;
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < events_per_sample; ++i) {
          const mmsim::StrategyState st{static_cast<mmsim::Qty>((i % 200) - 100), 300, -0.2};
          const auto q = strat->compute_quotes(sig, st);
          acc += static_cast<std::uint64_t>(q.bid_price_ticks + q.ask_price_ticks);
        }
        return acc;
      }));

  // 4. Stage: Pre-Trade Risk Gate
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "04_pretrade_risk", "risk_gate_check", events_per_sample, bcfg, [&]() {
        PreTradeRiskGate risk(RiskConfig{}, insts);
        TopOfBook tob{};
        tob.bid = LevelView{Price{9999}, Quantity{100}, 1};
        tob.ask = LevelView{Price{10001}, Quantity{100}, 1};
        ExposureSnapshot exp{};
        std::uint64_t ok_count = 0;
        for (std::size_t i = 0; i < events_per_sample; ++i) {
          OrderIntent oi{};
          oi.action = OrderAction::New;
          oi.client_order_id = i + 1;
          oi.token = ClientToken::from_uint64("RSK", i + 1);
          oi.side = Side::Buy;
          oi.order_type = OrderType::Limit;
          oi.tif = TimeInForce::GTC;
          oi.limit_price = Price{10000};
          oi.qty = Quantity{10};
          if (risk.check_order(0, 0, oi, tob, true, exp).approved) ++ok_count;
        }
        return ok_count;
      }));

  // 5. Stage: Smart Order Router Decision (Project 07 RoutePlanner across 3 venues)
  std::vector<VenueMeta> sor_venues(3);
  sor_venues[0] = VenueMeta{0, "V0", -20, 30, 50'000, 80'000, 50'000, 50'000'000, true};
  sor_venues[1] = VenueMeta{1, "V1", 20, -15, 60'000, 90'000, 60'000, 50'000'000, true};
  sor_venues[2] = VenueMeta{2, "V2", 0, 9, 150'000, 200'000, 150'000, 50'000'000, true};
  adapters::SmartRouterAdapter bench_router(insts, sor_venues, sor::RoutingPolicy::SmartCostAware);
  bench_router.update_venue_depth(0, 0, {{Price{9999}, Quantity{100}, 1}},
                                  {{Price{10001}, Quantity{100}, 1}}, SimTimeNs{1000},
                                  SimTimeNs{1000});
  bench_router.update_venue_depth(0, 1, {{Price{9999}, Quantity{100}, 1}},
                                  {{Price{10001}, Quantity{80}, 1}}, SimTimeNs{1000},
                                  SimTimeNs{1000});
  bench_router.update_venue_depth(0, 2, {{Price{9998}, Quantity{200}, 1}},
                                  {{Price{10002}, Quantity{200}, 1}}, SimTimeNs{1000},
                                  SimTimeNs{1000});
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "05_smart_routing", "sor_cost_aware_3v", events_per_sample, bcfg, [&]() {
        sor::ConsolidatedBook cbook;
        std::vector<sor::VenueConfig> vcs(3);
        for (std::uint8_t v = 0; v < 3; ++v) {
          vcs[v].id = v;
          vcs[v].name = sor_venues[v].name;
          vcs[v].taker_fee_bps = static_cast<double>(sor_venues[v].taker_fee_bps_x100) / 100.0;
          vcs[v].base_one_way_latency_ns = sor_venues[v].order_latency_ns;
        }
        cbook.configure_venues(vcs);
        cbook.set_level(0, sor::Side::Sell, 1'000'100, 100, 1000, 1000);
        cbook.set_level(1, sor::Side::Sell, 1'000'100, 80, 1000, 1000);
        cbook.set_level(2, sor::Side::Sell, 1'000'200, 200, 1000, 1000);
        sor::RoutePlanner planner(sor::RouterConfig{});
        sor::RouteRequest req{};
        req.side = sor::Side::Buy;
        req.qty = 120;
        req.limit_price = 1'000'200;
        req.tif = sor::TimeInForce::IOC;
        req.now_ns = 1500;
        std::uint64_t slices_acc = 0;
        for (std::size_t i = 0; i < events_per_sample; ++i) {
          const auto plan = planner.plan(cbook, nullptr, req);
          slices_acc += plan.slice_count;
        }
        return slices_acc;
      }));

  // 6. Stage: OUCH Wire Serialization + Decode Round-trip (Project 03)
  adapters::OuchProtocolAdapter ouch_proto(insts);
  OrderIntent sample_intent{};
  sample_intent.action = OrderAction::New;
  sample_intent.client_order_id = 1;
  sample_intent.token = ClientToken::from_uint64("PRF", 1);
  sample_intent.side = Side::Buy;
  sample_intent.order_type = OrderType::Limit;
  sample_intent.tif = TimeInForce::GTC;
  sample_intent.limit_price = Price{10'000};
  sample_intent.qty = Quantity{50};
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "06_wire_serialization", "ouch_roundtrip", events_per_sample, bcfg, [&]() {
        std::uint64_t acc = 0;
        std::vector<std::uint8_t> wire;
        ouch::wire::InboundMessage in_msg;
        ouch::wire::InboundMessage dec_msg;
        for (std::size_t i = 0; i < events_per_sample; ++i) {
          (void)ouch_proto.to_ouch_inbound(0, sample_intent, in_msg, &wire);
          (void)ouch::wire::decode_inbound(wire, dec_msg);
          acc += wire.size();
        }
        return acc;
      }));

  // 7. Stage: Matching Engine Steady-State A/B (Project 01 PoolBook vs MapBook — preallocated!)
  // Uses a 10,000-event steady-state workload and preallocated books outside the inner replay
  // (plus a separately sized cold-start comparison) to isolate hot-path matching performance.
  const auto steady_feed = adapters::BacktesterFeedAdapter::generate_l3_envelopes(
      0, 10'000, 1, 77, 10'000);
  lpl::BenchmarkConfig match_cfg = bcfg;
  match_cfg.ops_per_sample = steady_feed.size();
  match_cfg.samples_per_rep = std::max<std::size_t>(5, samples_per_rep / 2);

  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "07_matching_steady", "pool_book_10k", steady_feed.size(), match_cfg, [&]() {
        adapters::MatchingEngineAdapter ex(venue, insts, adapters::BookBackend::Pool, 4096);
        for (const auto& env : steady_feed) {
          ex.apply_market_event(0, std::get<MarketDataEvent>(env.payload), env.event_time_ns);
        }
        return ex.book_hash(0);
      }));
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "07_matching_steady", "map_book_10k", steady_feed.size(), match_cfg, [&]() {
        adapters::MatchingEngineAdapter ex(venue, insts, adapters::BookBackend::Map, 4096);
        for (const auto& env : steady_feed) {
          ex.apply_market_event(0, std::get<MarketDataEvent>(env.payload), env.event_time_ns);
        }
        return ex.book_hash(0);
      }));

  // 8. Stage: OMS + Fixed-Point Accounting Ledger Update
  out.stage_benchmarks.push_back(lpl::run_benchmark(
      "stage", "08_oms_and_ledger", "fixed_point_ledger", events_per_sample, bcfg, [&]() {
        AccountingLedger ldg(insts);
        OmsReconciler oms_local(ldg);
        for (std::size_t i = 0; i < events_per_sample; ++i) {
          const SimTimeNs t{static_cast<std::int64_t>(i + 1) * 1000};
          OrderIntent oi{};
          oi.action = OrderAction::New;
          oi.client_order_id = i + 1;
          oi.token = ClientToken::from_uint64("OMS", i + 1);
          oi.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
          oi.limit_price = Price{10'000};
          oi.qty = Quantity{10};
          (void)oms_local.on_order_sent(0, 0, oi, t);
          ExecutionReport fill{};
          fill.type = ExecReportType::Filled;
          fill.token = oi.token;
          fill.exec_id = i + 1;
          fill.side = oi.side;
          fill.exec_price = Price{10'000};
          fill.requested_qty = Quantity{10};
          fill.fill_qty = Quantity{10};
          fill.cum_filled_qty = Quantity{10};
          fill.leaves_qty = Quantity{0};
          fill.fee_scaled = Money{30};
          fill.exchange_time_ns = t;
          (void)oms_local.on_execution_report(0, 0, fill, t);
        }
        return static_cast<std::uint64_t>(ldg.reconcile().net_equity_cash_plus_inv.scaled);
      }));

  // 9. E2E Comparative Investigation A: Full Deterministic Pipeline (PoolBook vs MapBook)
  DeterministicEnginePipeline pipe;
  const auto golden_feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  lpl::BenchmarkConfig e2e_cfg = bcfg;
  e2e_cfg.ops_per_sample = golden_feed.size();

  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "full_pipeline_deterministic", "direct_pool_book", golden_feed.size(), e2e_cfg,
      [&]() {
        ScenarioConfig sc{};
        sc.book_backend = adapters::BookBackend::Pool;
        adapters::SimpleDeterministicStrategy strat;
        const auto r = pipe.run_single_venue(sc, golden_feed, strat, insts, venue);
        out.pool_checksum = r.replay_checksum;
        return r.replay_checksum;
      }));

  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "full_pipeline_deterministic", "direct_map_book", golden_feed.size(), e2e_cfg,
      [&]() {
        ScenarioConfig sc{};
        sc.book_backend = adapters::BookBackend::Map;
        adapters::SimpleDeterministicStrategy strat;
        const auto r = pipe.run_single_venue(sc, golden_feed, strat, insts, venue);
        out.map_checksum = r.replay_checksum;
        return r.replay_checksum;
      }));
  out.pool_vs_map_checksum_match = (out.pool_checksum == out.map_checksum);

  // 10. E2E Comparative Investigation B: Direct In-Process vs SPSC Two-Thread Transport (Project 05)
  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "feed_transport_compare", "direct_in_process", canon_feed.size(), bcfg, [&]() {
        CanonicalOrderBook book;
        for (const auto& env : canon_feed) {
          book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
        }
        out.direct_checksum = book.state_hash();
        return out.direct_checksum;
      }));

  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "feed_transport_compare", "spsc_two_thread", canon_feed.size(), bcfg, [&]() {
        adapters::SpscTransportAdapter q(4096, QueueOverflowPolicy::Backpressure);
        std::atomic<bool> done{false};
        std::uint64_t consumer_hash = 0;

        std::thread consumer([&]() {
          CanonicalOrderBook book;
          Envelope env{};
          while (true) {
            if (q.try_pop(env)) {
              book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
            } else if (done.load(std::memory_order_acquire)) {
              while (q.try_pop(env)) {
                book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
              }
              break;
            } else {
              lfm::cpu_relax();
            }
          }
          consumer_hash = book.state_hash();
        });

        for (const auto& env : canon_feed) {
          (void)q.push(env);
        }
        done.store(true, std::memory_order_release);
        consumer.join();
        out.spsc_checksum = consumer_hash;
        out.spsc_drops = q.stats().dropped_for_resync;
        return consumer_hash;
      }));

  // 11. E2E Comparative Investigation C: Clean Multicast vs Faulty Recoverable Multicast (Project 06)
  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "multicast_mdf1_compare", "clean_no_loss", golden_feed.size(), e2e_cfg, [&]() {
        adapters::MulticastFeedAdapter::Config mc{};
        mc.channel_id = 1;
        adapters::MulticastFeedAdapter mdf_ad(mc);
        std::uint64_t t = 1'000'000ULL;
        for (std::size_t i = 0; i < golden_feed.size(); i += 4) {
          const std::size_t n = std::min<std::size_t>(4, golden_feed.size() - i);
          mdf_ad.publish_batch(std::span<const Envelope>(golden_feed.data() + i, n), t);
          t += 50'000ULL;
        }
        mdf_ad.flush_and_recover(t);
        return mdf_ad.summary().subscriber_state_hash;
      }));

  out.e2e_benchmarks.push_back(lpl::run_benchmark(
      "e2e", "multicast_mdf1_compare", "loss_15pct_recovered", golden_feed.size(), e2e_cfg, [&]() {
        adapters::MulticastFeedAdapter::Config mc{};
        mc.channel_id = 1;
        mc.fault_config.seed = 42;
        mc.fault_config.drop_prob = 0.15;
        mc.fault_config.reorder_prob = 0.10;
        mc.fault_config.max_reorder_delay = 2;
        adapters::MulticastFeedAdapter mdf_ad(mc);
        std::uint64_t t = 1'000'000ULL;
        for (std::size_t i = 0; i < golden_feed.size(); i += 4) {
          const std::size_t n = std::min<std::size_t>(4, golden_feed.size() - i);
          mdf_ad.publish_batch(std::span<const Envelope>(golden_feed.data() + i, n), t);
          t += 50'000ULL;
        }
        mdf_ad.flush_and_recover(t);
        return mdf_ad.summary().subscriber_state_hash;
      }));

  // 7. Separate wall-clock E2E distribution measurements:
  //    tick-to-decision, order-submit-to-ack, and order-submit-to-fill.
  std::vector<double> t2d_samples;
  std::vector<double> sub2ack_samples;
  std::vector<double> sub2fill_samples;
  t2d_samples.reserve(1000);
  sub2ack_samples.reserve(1000);
  sub2fill_samples.reserve(1000);

  adapters::MatchingEngineAdapter ex(venue, insts, adapters::BookBackend::Pool);
  CanonicalOrderBook obs_book;
  AccountingLedger ledger(insts);
  OmsReconciler oms(ledger);
  PreTradeRiskGate risk(RiskConfig{}, insts);

  // Seed resting ask liquidity for submit-to-fill measurements.
  for (std::uint64_t i = 1; i <= 200; ++i) {
    MarketDataEvent md{};
    md.kind = MdUpdateKind::Add;
    md.feed_order_ref = FeedOrderRef{i};
    md.side = (i <= 100) ? Side::Buy : Side::Sell;
    md.price = (i <= 100) ? Price{9999} : Price{10001};
    md.qty = Quantity{500};
    md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(i * 1000)};
    ex.apply_market_event(0, md, md.source_time_ns);
    obs_book.apply(md, md.source_time_ns);
  }

  // Warmup 100 iterations, then measure 500 samples.
  for (std::size_t iter = 0; iter < 600; ++iter) {
    const SimTimeNs sim_t{1'000'000LL + static_cast<std::int64_t>(iter * 1000)};
    MarketDataEvent tick{};
    tick.kind = MdUpdateKind::Add;
    tick.feed_order_ref = FeedOrderRef{1000 + iter};
    tick.side = Side::Buy;
    tick.price = Price{9998};
    tick.qty = Quantity{10};
    tick.source_time_ns = sim_t;

    const WallTimeNs w0 = adapters::PerfLabAdapter::now_wall_ns();
    obs_book.apply(tick, sim_t);
    const TopOfBook tob = obs_book.top_of_book(sim_t);
    OrderIntent intent{};
    intent.action = OrderAction::New;
    intent.client_order_id = iter + 1;
    intent.token = ClientToken::from_uint64("LAT", iter + 1);
    intent.side = (iter % 2 == 0) ? Side::Buy : Side::Sell;
    intent.order_type = OrderType::Limit;
    intent.tif = TimeInForce::IOC;
    intent.limit_price = (intent.side == Side::Buy) ? tob.ask->price : tob.bid->price;
    intent.qty = Quantity{1};
    const RiskDecision rd = risk.check_order(0, 0, intent, tob, true, oms.exposure(0));
    const WallTimeNs w1 = adapters::PerfLabAdapter::now_wall_ns();

    if (rd.approved) {
      const WallTimeNs ws0 = adapters::PerfLabAdapter::now_wall_ns();
      (void)oms.on_order_sent(0, 0, intent, sim_t);
      std::vector<ExecutionReport> reports;
      (void)ex.submit_order(0, intent, sim_t, reports);
      WallTimeNs wack = ws0;
      WallTimeNs wfill = ws0;
      bool got_ack = false;
      bool got_fill = false;
      for (const auto& r : reports) {
        (void)oms.on_execution_report(0, 0, r, sim_t);
        if (!got_ack && r.type == ExecReportType::Accepted) {
          wack = adapters::PerfLabAdapter::now_wall_ns();
          got_ack = true;
        }
        if (!got_fill && (r.type == ExecReportType::PartiallyFilled ||
                          r.type == ExecReportType::Filled)) {
          wfill = adapters::PerfLabAdapter::now_wall_ns();
          got_fill = true;
        }
      }
      if (iter >= 100) {
        t2d_samples.push_back(static_cast<double>(w1 - w0));
        if (got_ack) sub2ack_samples.push_back(static_cast<double>(wack - ws0));
        if (got_fill) sub2fill_samples.push_back(static_cast<double>(wfill - ws0));
      }
    }
  }

  out.tick_to_decision_ns = adapters::PerfLabAdapter::summarize_ns(std::move(t2d_samples));
  out.order_submit_to_ack_ns = adapters::PerfLabAdapter::summarize_ns(std::move(sub2ack_samples));
  out.order_submit_to_fill_ns = adapters::PerfLabAdapter::summarize_ns(std::move(sub2fill_samples));
  return out;
}

}  // namespace hft::runtime
