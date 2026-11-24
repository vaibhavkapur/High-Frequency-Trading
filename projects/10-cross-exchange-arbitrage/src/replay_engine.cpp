#include "xarb/replay_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <random>

namespace xarb {

ReplayEngine::ReplayEngine(const InstrumentRegistry& registry, ReplayConfig cfg)
    : registry_(registry), cfg_(std::move(cfg)) {}

bool ReplayEngine::is_in_injected_outage(VenueId v, TimestampNs ts_ns) const noexcept {
  for (const auto& w : cfg_.injected_outages) {
    if (w.venue == v && ts_ns >= w.start_ns && ts_ns <= w.end_ns) {
      return true;
    }
  }
  return false;
}

double ReplayEngine::compute_mark_mid_quote(
    uint32_t inst_id, const std::vector<std::array<std::unique_ptr<VenueAdapter>, kVenueCount>>& books) const {
  const InstrumentSpec* inst = registry_.get(inst_id);
  if (!inst || inst_id >= books.size()) return 0.0;
  double sum = 0.0;
  int count = 0;
  for (size_t v = 0; v < kVenueCount; ++v) {
    if (!books[inst_id][v]) continue;
    auto b = books[inst_id][v]->book().best_bid();
    auto a = books[inst_id][v]->book().best_ask();
    if (b && a && b->price > 0 && a->price > 0) {
      sum += 0.5 * (inst->ticks_to_price(b->price) + inst->ticks_to_price(a->price));
      ++count;
    }
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

ReplayResult ReplayEngine::run(const std::vector<RecordedFeedEvent>& feed, const std::string& feed_path) {
  ReplayResult out;
  out.summary.feed_path = feed_path;
  out.summary.scenario_name = cfg_.scenario_name;
  out.summary.total_events = feed.size();

  const size_t n_inst = registry_.size();
  std::vector<std::array<std::unique_ptr<VenueAdapter>, kVenueCount>> truth_adapters(n_inst);
  std::vector<std::array<std::unique_ptr<VenueAdapter>, kVenueCount>> obs_adapters(n_inst);

  for (size_t i = 0; i < n_inst; ++i) {
    const auto& spec = *registry_.get(static_cast<uint32_t>(i));
    for (size_t v = 0; v < kVenueCount; ++v) {
      auto vid = static_cast<VenueId>(v);
      truth_adapters[i][v] = make_venue_adapter(vid, spec, cfg_.max_stale_ns, cfg_.max_book_depth);
      obs_adapters[i][v] = make_venue_adapter(vid, spec, cfg_.max_stale_ns, cfg_.max_book_depth);
    }
  }

  OpportunityScanner scanner(cfg_.scanner);
  PaperExecutor paper(registry_, cfg_.paper);
  std::mt19937_64 rng(cfg_.rng_seed);

  std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, std::greater<ScheduledEvent>> pq;
  uint64_t next_seq = 1;

  TimestampNs min_ts = 0;
  TimestampNs max_ts = 0;
  if (!feed.empty()) {
    min_ts = feed.front().recv_ns;
    max_ts = feed.front().recv_ns;
    for (size_t i = 0; i < feed.size(); ++i) {
      const auto& ev = feed[i];
      if (ev.recv_ns < min_ts) min_ts = ev.recv_ns;
      if (ev.recv_ns > max_ts) max_ts = ev.recv_ns;

      ScheduledEvent t_ev;
      t_ev.ts_ns = ev.recv_ns;
      t_ev.kind = EvKind::TruthFeed;
      t_ev.seq = next_seq++;
      t_ev.feed_idx = i;
      pq.push(t_ev);

      ScheduledEvent o_ev;
      o_ev.ts_ns = ev.recv_ns + cfg_.feed_delay_ns[static_cast<size_t>(ev.venue)];
      o_ev.kind = EvKind::ObservedFeed;
      o_ev.seq = next_seq++;
      o_ev.feed_idx = i;
      pq.push(o_ev);
    }

    if (cfg_.health_check_interval_ns > 0) {
      for (TimestampNs t = min_ts + cfg_.health_check_interval_ns; t <= max_ts + cfg_.max_stale_ns;
           t += cfg_.health_check_interval_ns) {
        ScheduledEvent h_ev;
        h_ev.ts_ns = t;
        h_ev.kind = EvKind::HealthTimer;
        h_ev.seq = next_seq++;
        pq.push(h_ev);
      }
    }
  }

  auto dispatch_adapter = [](VenueAdapter& adapter, const RecordedFeedEvent& ev, TimestampNs now_ns) -> AdapterAction {
    switch (ev.kind) {
      case FeedEventKind::WsMessage:
        return adapter.on_ws_message(ev.payload, now_ns);
      case FeedEventKind::RestSnapshot:
        return adapter.on_rest_snapshot(ev.payload, now_ns);
      case FeedEventKind::Disconnect:
        return adapter.on_disconnect(now_ns, ev.payload.empty() ? "feed_disconnect" : ev.payload);
      case FeedEventKind::Reconnect:
        return adapter.on_reconnect(now_ns);
    }
    return {};
  };

  auto sample_order_arrival = [&](TimestampNs now_ns, VenueId v) -> TimestampNs {
    TimestampNs base = cfg_.paper.order_latency_ns[static_cast<size_t>(v)];
    TimestampNs jitter = 0;
    if (cfg_.paper.order_jitter_ns > 0) {
      std::uniform_int_distribution<TimestampNs> dist(-cfg_.paper.order_jitter_ns, cfg_.paper.order_jitter_ns);
      jitter = dist(rng);
    }
    return now_ns + std::max<TimestampNs>(0, base + jitter);
  };

  std::vector<double> proc_lat_ns;
  if (cfg_.measure_processing_latency) {
    proc_lat_ns.reserve(std::min<size_t>(feed.size(), 250000));
  }

  double initial_mark_mid = 0.0;
  double last_mark_mid = 0.0;
  uint64_t total_book_updates = 0;
  Fnv1a64 digest;

  const auto wall_start = std::chrono::steady_clock::now();

  while (!pq.empty()) {
    const ScheduledEvent ev = pq.top();
    pq.pop();

    if (ev.kind == EvKind::TruthFeed) {
      const auto& fe = feed[ev.feed_idx];
      if (fe.instrument_id < n_inst) {
        auto& adapter = *truth_adapters[fe.instrument_id][static_cast<size_t>(fe.venue)];
        dispatch_adapter(adapter, fe, ev.ts_ns);
        if (initial_mark_mid == 0.0) {
          initial_mark_mid = compute_mark_mid_quote(fe.instrument_id, truth_adapters);
        }
        double m = compute_mark_mid_quote(fe.instrument_id, truth_adapters);
        if (m > 0.0) last_mark_mid = m;
      }
      continue;
    }

    if (ev.kind == EvKind::ObservedFeed) {
      const auto& fe = feed[ev.feed_idx];
      if (fe.instrument_id >= n_inst) continue;
      if (is_in_injected_outage(fe.venue, ev.ts_ns)) continue;

      const auto t0 = cfg_.measure_processing_latency ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};

      auto& adapter = *obs_adapters[fe.instrument_id][static_cast<size_t>(fe.venue)];
      const AdapterAction act = dispatch_adapter(adapter, fe, ev.ts_ns);
      if (act.book_updated) {
        total_book_updates += 1;
        const auto* spec = registry_.get(fe.instrument_id);
        std::array<const VenueAdapter*, kVenueCount> ptrs{};
        for (size_t v = 0; v < kVenueCount; ++v) {
          ptrs[v] = obs_adapters[fe.instrument_id][v].get();
        }
        auto sigs = scanner.scan_instrument(ev.ts_ns, *spec, ptrs, [&](VenueId bv, VenueId sv) {
          return paper.funding_for_route(fe.instrument_id, bv, sv);
        });

        for (const auto& s : sigs) {
          out.signals.push_back(s);
          digest.update_u64(s.signal_id);
          digest.update_i64(s.detect_ns);
          digest.update_u64(static_cast<uint64_t>(s.status));
          digest.update_i64(s.funded_executable_lots);

          if (s.is_actionable()) {
            const TimestampNs buy_arr = sample_order_arrival(ev.ts_ns, s.buy_venue);
            const TimestampNs sell_arr = sample_order_arrival(ev.ts_ns, s.sell_venue);
            auto legs = paper.submit_signal(s, buy_arr, sell_arr);
            for (const auto& leg : legs) {
              ScheduledEvent leg_ev;
              leg_ev.ts_ns = leg.arrival_ns;
              leg_ev.kind = EvKind::OrderArrival;
              leg_ev.seq = next_seq++;
              leg_ev.leg = leg;
              pq.push(leg_ev);
            }
          }
        }
      }

      if (cfg_.measure_processing_latency && proc_lat_ns.size() < 250000) {
        const auto t1 = std::chrono::steady_clock::now();
        proc_lat_ns.push_back(
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
      }
      continue;
    }

    if (ev.kind == EvKind::OrderArrival) {
      const auto& leg = ev.leg;
      if (leg.instrument_id < n_inst) {
        const auto& truth_book = truth_adapters[leg.instrument_id][static_cast<size_t>(leg.venue)]->book();
        double mark_mid = compute_mark_mid_quote(leg.instrument_id, truth_adapters);
        if (mark_mid <= 0.0) mark_mid = last_mark_mid;
        const bool finished = paper.execute_leg(leg, truth_book, mark_mid);
        if (finished) {
          auto xfer = paper.maybe_initiate_rebalance(leg.instrument_id, ev.ts_ns);
          if (xfer) {
            ScheduledEvent x_ev;
            x_ev.ts_ns = xfer->arrival_ns;
            x_ev.kind = EvKind::RebalanceArrival;
            x_ev.seq = next_seq++;
            x_ev.xfer = *xfer;
            pq.push(x_ev);
          }
        }
      }
      continue;
    }

    if (ev.kind == EvKind::RebalanceArrival) {
      double mark_mid = compute_mark_mid_quote(ev.xfer.instrument_id, truth_adapters);
      if (mark_mid <= 0.0) mark_mid = last_mark_mid;
      paper.complete_rebalance(ev.xfer, mark_mid);
      continue;
    }

    if (ev.kind == EvKind::HealthTimer) {
      for (size_t i = 0; i < n_inst; ++i) {
        for (size_t v = 0; v < kVenueCount; ++v) {
          obs_adapters[i][v]->check_staleness(ev.ts_ns);
        }
      }
      continue;
    }
  }

  const auto wall_end = std::chrono::steady_clock::now();
  const double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

  out.trades = paper.trades();
  out.summary.simulated_span_sec =
      (max_ts > min_ts) ? static_cast<double>(max_ts - min_ts) / 1e9 : 0.0;
  out.summary.wall_time_sec = wall_sec;
  out.summary.events_per_sec = wall_sec > 0.0 ? static_cast<double>(feed.size()) / wall_sec : 0.0;
  out.summary.book_updates_per_sec = wall_sec > 0.0 ? static_cast<double>(total_book_updates) / wall_sec : 0.0;
  out.summary.scanner = scanner.stats();
  out.summary.paper = paper.stats();
  out.summary.processing_latency_ns = compute_percentiles(std::move(proc_lat_ns));

  // Mean gross vs net spreads across signals.
  double sum_gross = 0.0;
  double sum_net = 0.0;
  size_t net_cnt = 0;
  for (const auto& s : out.signals) {
    sum_gross += s.gross_spread_bps;
    if (s.funded_executable_lots > 0) {
      sum_net += s.net_edge_bps;
      ++net_cnt;
    }
  }
  if (!out.signals.empty()) {
    out.summary.mean_gross_spread_bps = sum_gross / static_cast<double>(out.signals.size());
  }
  if (net_cnt > 0) {
    out.summary.mean_net_edge_bps = sum_net / static_cast<double>(net_cnt);
  }

  const auto& ps = out.summary.paper;
  if (ps.trades_attempted > 0) {
    out.summary.fill_rate = static_cast<double>(ps.both_filled_profitable + ps.both_filled_loss_slippage) /
                            static_cast<double>(ps.trades_attempted);
  }
  if (ps.trades_completed > 0) {
    const uint64_t fp = ps.both_filled_loss_slippage + ps.one_legged_unhedged + ps.both_missed_expired;
    out.summary.false_positive_rate = static_cast<double>(fp) / static_cast<double>(ps.trades_completed);
  }

  // Final equity & accounting invariant verification across instruments (primary instrument 0).
  const double mark_final = last_mark_mid > 0.0 ? last_mark_mid : initial_mark_mid;
  bool all_ok = true;
  double init_eq = 0.0;
  double fin_eq = 0.0;
  for (size_t i = 0; i < n_inst; ++i) {
    const double m = compute_mark_mid_quote(static_cast<uint32_t>(i), truth_adapters);
    const double use_m = m > 0.0 ? m : mark_final;
    if (use_m > 0.0) {
      init_eq += paper.initial_equity_quote(static_cast<uint32_t>(i), use_m);
      fin_eq += paper.total_equity_quote(static_cast<uint32_t>(i), use_m);
    }
    if (!paper.verify_invariants(static_cast<uint32_t>(i))) {
      all_ok = false;
    }
  }
  out.summary.initial_equity_quote = init_eq;
  out.summary.final_equity_quote = fin_eq;
  out.summary.equity_delta_quote = fin_eq - init_eq;
  out.summary.accounting_invariants_ok = all_ok;

  // Aggregate per-venue adapter metrics.
  for (size_t v = 0; v < kVenueCount; ++v) {
    VenueRunReport& vr = out.summary.venues[v];
    vr.venue = static_cast<VenueId>(v);
    std::vector<double> recon_ms;
    std::vector<double> stale_ms;
    for (size_t i = 0; i < n_inst; ++i) {
      const auto& m = obs_adapters[i][v]->metrics();
      vr.messages_received += m.messages_received;
      vr.deltas_applied += m.deltas_applied;
      vr.snapshots_applied += m.snapshots_applied;
      vr.heartbeats_received += m.heartbeats_received;
      vr.gaps_detected += m.gaps_detected;
      vr.checksum_mismatches += m.checksum_mismatches;
      vr.disconnects += m.disconnects;
      vr.resync_count += m.resync_count;
      vr.stale_transitions += m.stale_transitions;
      vr.total_stale_ms += static_cast<double>(m.total_stale_ns) / 1e6;
      vr.total_resync_ms += static_cast<double>(m.total_resync_ns) / 1e6;
      for (TimestampNs d : m.reconnect_durations_ns) recon_ms.push_back(static_cast<double>(d) / 1e6);
      for (TimestampNs s : m.staleness_samples_ns) stale_ms.push_back(static_cast<double>(s) / 1e6);
    }
    vr.reconnect_duration_ms = compute_percentiles(std::move(recon_ms));
    vr.staleness_ms = compute_percentiles(std::move(stale_ms));
  }

  // Hash every trade outcome and final stats for deterministic replay verification.
  for (const auto& t : out.trades) {
    digest.update_u64(t.trade_id);
    digest.update_u64(static_cast<uint64_t>(t.outcome));
    digest.update_i64(t.buy_filled_lots);
    digest.update_i64(t.sell_filled_lots);
    digest.update_i64(static_cast<int64_t>(std::llround(t.total_pnl_quote * 1e4)));
  }
  digest.update_u64(out.summary.scanner.gross_crosses_seen);
  digest.update_u64(out.summary.scanner.actionable_signals);
  out.summary.run_hash = digest.state;

  return out;
}

}  // namespace xarb
