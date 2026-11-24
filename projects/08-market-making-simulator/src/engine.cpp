#include "mmsim/engine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <queue>
#include <unordered_map>

namespace mmsim {

namespace {

enum class SimEventKind : uint8_t {
    ExchangeOrderArrival = 0,
    ExchangeCancelArrival = 1,
    MarketStrategyEvent = 2,
    StrategyOrderAck = 3,
    StrategyCancelAck = 4,
    StrategyFillArrival = 5,
};

struct ScheduledEvent {
    TimestampNs timestamp_ns = 0;
    uint8_t priority = 0;
    uint64_t seq = 0;
    SimEventKind kind = SimEventKind::MarketStrategyEvent;

    MarketEvent market_ev{};
    OrderRequest order_req{};
    CancelRequest cancel_req{};
    OrderAck order_ack{};
    CancelAck cancel_ack{};
    FillEvent fill_ev{};
};

struct ScheduledEventGreater {
    bool operator()(const ScheduledEvent& a, const ScheduledEvent& b) const noexcept {
        if (a.timestamp_ns != b.timestamp_ns) {
            return a.timestamp_ns > b.timestamp_ns;
        }
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.seq > b.seq;
    }
};

struct OmsOrder {
    OrderId order_id = 0;
    Side side = Side::Bid;
    PriceTicks price_ticks = 0;
    Qty orig_qty = 0;
    Qty remaining_qty = 0;
    OrderStatus status = OrderStatus::PendingNew;
    TimestampNs created_ns = 0;
    TimestampNs cancel_sent_ns = -1;
};

} // namespace

SimulatorEngine::SimulatorEngine(const SimConfig& cfg) : cfg_(cfg) {}

SimResult SimulatorEngine::run(const std::vector<MarketEvent>& events, bool record_timeseries) {
    SimResult result{};
    result.summary.strategy_name = strategy_type_to_string(cfg_.strategy.type);
    result.summary.fill_model_name = fill_model_to_string(cfg_.exchange.fill_model);
    result.summary.book_impl_name = book_impl_to_string(cfg_.book_impl);
    result.summary.total_market_events = events.size();

    if (events.empty()) {
        return result;
    }

    auto exchange_book = make_book(cfg_.book_impl);
    auto strategy_book = make_book(cfg_.book_impl);
    FeatureEngine features(cfg_.features);
    auto strategy = make_strategy(cfg_.strategy);
    RiskManager risk(cfg_.risk);
    risk.reset(cfg_.initial_cash_dollars);

    SimulatedExchange exchange(cfg_.exchange, cfg_.fees, cfg_.tick_size_dollars);
    Portfolio exch_portfolio(cfg_.tick_size_dollars, cfg_.initial_cash_dollars);
    Portfolio strat_portfolio(cfg_.tick_size_dollars, cfg_.initial_cash_dollars);

    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, ScheduledEventGreater> queue;
    uint64_t sched_seq = 1;
    OrderId next_order_id = 1;

    std::unordered_map<OrderId, OmsOrder> oms_orders;
    OrderId active_bid_id = 0;
    OrderId active_ask_id = 0;

    std::vector<FillEvent> all_fills;
    std::vector<MidSample> mid_timeline;
    if (record_timeseries) {
        mid_timeline.reserve(events.size() / 2 + 16);
    }

    std::vector<FillEvent> step_fills;
    step_fills.reserve(8);

    const TimestampNs start_ns = events.front().timestamp_ns;
    const TimestampNs end_ns = events.back().timestamp_ns;
    result.summary.session_start_ns = start_ns;
    result.summary.session_end_ns = end_ns;
    result.summary.session_duration_sec =
        static_cast<double>(std::max<TimestampNs>(0, end_ns - start_ns)) * 1e-9;

    // Time-weighted inventory & equity sampling state (ground-truth exchange clock).
    TimestampNs last_inv_ts = start_ns;
    double weighted_abs_pos = 0.0;
    double weighted_sq_pos = 0.0;
    TimestampNs next_equity_sample_ns = start_ns;
    double last_exch_mid_ticks = 0.0;
    bool have_exch_mid = false;

    auto accumulate_inventory = [&](TimestampNs now_ns) {
        if (now_ns > last_inv_ts) {
            const double dt = static_cast<double>(now_ns - last_inv_ts);
            const double pos = static_cast<double>(exch_portfolio.position());
            weighted_abs_pos += std::abs(pos) * dt;
            weighted_sq_pos += pos * pos * dt;
            last_inv_ts = now_ns;
        }
    };

    auto maybe_record_equity = [&](TimestampNs now_ns, bool force) {
        if (!record_timeseries || !have_exch_mid) {
            return;
        }
        if (force || now_ns >= next_equity_sample_ns) {
            const double mark = last_exch_mid_ticks * cfg_.tick_size_dollars;
            EquityPoint pt{};
            pt.timestamp_ns = now_ns;
            pt.mid_ticks = last_exch_mid_ticks;
            pt.position = exch_portfolio.position();
            pt.cash_dollars = exch_portfolio.cash_dollars();
            pt.realized_pnl_dollars = exch_portfolio.realized_pnl_dollars();
            pt.unrealized_pnl_dollars = exch_portfolio.unrealized_pnl_dollars(mark);
            pt.total_fees_dollars = exch_portfolio.total_fees_dollars();
            pt.gross_mtm_pnl_dollars = exch_portfolio.gross_mtm_pnl_dollars(mark);
            pt.net_mtm_pnl_dollars = exch_portfolio.net_mtm_pnl_dollars(mark);
            result.equity_curve.push_back(pt);
            if (cfg_.equity_sample_interval_ns > 0) {
                while (next_equity_sample_ns <= now_ns) {
                    next_equity_sample_ns += cfg_.equity_sample_interval_ns;
                }
            }
        }
    };

    auto compute_risk_state = [&](double mark_price_dollars) -> RiskState {
        RiskState rs{};
        rs.confirmed_position = strat_portfolio.position();
        rs.mark_price_dollars = mark_price_dollars;
        rs.current_net_equity_dollars = strat_portfolio.net_equity_dollars(mark_price_dollars);
        for (const auto& [id, o] : oms_orders) {
            if (o.status == OrderStatus::PendingNew || o.status == OrderStatus::Live ||
                o.status == OrderStatus::PendingCancel) {
                ++rs.open_order_count;
                if (o.side == Side::Bid) {
                    rs.open_buy_qty += o.remaining_qty;
                } else {
                    rs.open_sell_qty += o.remaining_qty;
                }
            }
        }
        return rs;
    };

    auto send_cancel = [&](TimestampNs now_ns, OrderId oid) {
        auto it = oms_orders.find(oid);
        if (it == oms_orders.end()) {
            return;
        }
        if (it->second.status != OrderStatus::Live && it->second.status != OrderStatus::PendingNew) {
            return;
        }
        it->second.status = OrderStatus::PendingCancel;
        it->second.cancel_sent_ns = now_ns;
        ++result.summary.cancels_submitted;

        ScheduledEvent se{};
        se.timestamp_ns = now_ns + cfg_.latency.order_latency_ns;
        se.priority = 1;
        se.seq = sched_seq++;
        se.kind = SimEventKind::ExchangeCancelArrival;
        se.cancel_req = CancelRequest{oid, now_ns};
        queue.push(se);
    };

    auto cancel_all_open = [&](TimestampNs now_ns) {
        for (auto& [id, o] : oms_orders) {
            if (o.status == OrderStatus::Live || o.status == OrderStatus::PendingNew) {
                send_cancel(now_ns, id);
            }
        }
    };

    auto submit_order = [&](TimestampNs now_ns, Side side, PriceTicks price_ticks, Qty qty,
                            double mark_price_dollars) -> OrderId {
        OrderRequest req{};
        req.order_id = next_order_id++;
        req.submit_time_ns = now_ns;
        req.side = side;
        req.price_ticks = price_ticks;
        req.qty = qty;
        req.post_only = cfg_.strategy.post_only;

        const RiskState rs = compute_risk_state(mark_price_dollars);
        const RejectReason r = risk.check_order(req, rs);
        if (r != RejectReason::None) {
            ++result.summary.quotes_risk_rejected;
            return 0;
        }

        ++result.summary.quotes_submitted;
        result.summary.total_quoted_shares += qty;

        OmsOrder o{};
        o.order_id = req.order_id;
        o.side = side;
        o.price_ticks = price_ticks;
        o.orig_qty = qty;
        o.remaining_qty = qty;
        o.status = OrderStatus::PendingNew;
        o.created_ns = now_ns;
        oms_orders.emplace(req.order_id, o);

        ScheduledEvent se{};
        se.timestamp_ns = now_ns + cfg_.latency.order_latency_ns;
        se.priority = 0;
        se.seq = sched_seq++;
        se.kind = SimEventKind::ExchangeOrderArrival;
        se.order_req = req;
        queue.push(se);
        return req.order_id;
    };

    auto manage_side_quote = [&](TimestampNs now_ns, Side side, bool want_quote,
                                 PriceTicks target_px, Qty target_qty, double mark_price_dollars) {
        OrderId& slot = (side == Side::Bid) ? active_bid_id : active_ask_id;
        auto it = (slot != 0) ? oms_orders.find(slot) : oms_orders.end();

        if (it != oms_orders.end()) {
            const OmsOrder& cur = it->second;
            if (cur.status == OrderStatus::PendingCancel) {
                return; // Wait for cancel ack before quoting a replacement on this side.
            }
            if (!want_quote) {
                send_cancel(now_ns, slot);
                return;
            }
            const PriceTicks px_diff = std::abs(cur.price_ticks - target_px);
            const TimestampNs age_ns = now_ns - cur.created_ns;
            const bool qty_changed = (cur.orig_qty != target_qty);
            if (px_diff < cfg_.strategy.requote_threshold_ticks && !qty_changed) {
                return; // Within hysteresis band; preserve queue priority.
            }
            if (age_ns < cfg_.strategy.min_quote_lifetime_ns && px_diff <= 1) {
                return; // Respect minimum quote lifetime to avoid churn.
            }
            send_cancel(now_ns, slot);
            return;
        }

        slot = 0;
        if (want_quote && target_qty > 0 && target_px > 0) {
            slot = submit_order(now_ns, side, target_px, target_qty, mark_price_dollars);
        }
    };

    auto dispatch_exchange_fills = [&](TimestampNs now_ns) {
        for (auto& f : step_fills) {
            // Telemetry: mark fills that occurred after a strategy cancel was already in flight.
            auto oit = oms_orders.find(f.order_id);
            if (oit != oms_orders.end() && oit->second.cancel_sent_ns >= 0 &&
                oit->second.cancel_sent_ns <= now_ns) {
                f.raced_cancel = true;
                ++result.summary.cancel_races_filled;
            }

            accumulate_inventory(now_ns);
            exch_portfolio.on_fill(f);
            if (have_exch_mid) {
                exch_portfolio.update_drawdown(last_exch_mid_ticks * cfg_.tick_size_dollars);
            }
            all_fills.push_back(f);

            ScheduledEvent se{};
            se.timestamp_ns = now_ns + cfg_.latency.ack_latency_ns;
            se.priority = 3;
            se.seq = sched_seq++;
            se.kind = SimEventKind::StrategyFillArrival;
            se.fill_ev = f;
            queue.push(se);
        }
        step_fills.clear();
    };

    size_t next_market_idx = 0;

    while (next_market_idx < events.size() || !queue.empty()) {
        const bool take_market =
            next_market_idx < events.size() &&
            (queue.empty() || events[next_market_idx].timestamp_ns <= queue.top().timestamp_ns);

        if (take_market) {
            const MarketEvent& ev = events[next_market_idx++];
            const TimestampNs now_ns = ev.timestamp_ns;
            accumulate_inventory(now_ns);

            // 1. Apply market event on exchange book and run simulated exchange matching.
            step_fills.clear();
            if (ev.type == MarketEventType::BookClear) {
                exchange_book->clear();
            } else if (ev.type == MarketEventType::BookDelta) {
                const Qty old_qty = exchange_book->apply_delta(ev.side, ev.price_ticks, ev.qty);
                exchange.on_book_delta(now_ns, ev, old_qty, *exchange_book, step_fills);
            } else if (ev.type == MarketEventType::Trade) {
                exchange.on_trade(now_ns, ev, *exchange_book, step_fills);
            }

            const TopOfBook tob = exchange_book->top_of_book();
            if (tob.valid()) {
                last_exch_mid_ticks = tob.mid_ticks();
                have_exch_mid = true;
                if (record_timeseries) {
                    if (mid_timeline.empty() || mid_timeline.back().timestamp_ns != now_ns) {
                        mid_timeline.push_back(MidSample{now_ns, last_exch_mid_ticks});
                    } else {
                        mid_timeline.back().mid_ticks = last_exch_mid_ticks;
                    }
                }
                exch_portfolio.update_drawdown(last_exch_mid_ticks * cfg_.tick_size_dollars);
                maybe_record_equity(now_ns, false);
            }

            if (!step_fills.empty()) {
                dispatch_exchange_fills(now_ns);
            }

            // 2. Schedule delayed market data arrival at strategy (strictly causal).
            ScheduledEvent md{};
            md.timestamp_ns = now_ns + cfg_.latency.md_latency_ns;
            md.priority = 2;
            md.seq = sched_seq++;
            md.kind = SimEventKind::MarketStrategyEvent;
            md.market_ev = ev;
            queue.push(md);
            continue;
        }

        const ScheduledEvent se = queue.top();
        queue.pop();
        const TimestampNs now_ns = se.timestamp_ns;

        switch (se.kind) {
        case SimEventKind::ExchangeOrderArrival: {
            step_fills.clear();
            const OrderAck ack =
                exchange.on_new_order(now_ns, se.order_req, *exchange_book, step_fills);

            ScheduledEvent ack_ev{};
            ack_ev.timestamp_ns = now_ns + cfg_.latency.ack_latency_ns;
            ack_ev.priority = 2;
            ack_ev.seq = sched_seq++;
            ack_ev.kind = SimEventKind::StrategyOrderAck;
            ack_ev.order_ack = ack;
            queue.push(ack_ev);

            if (!step_fills.empty()) {
                dispatch_exchange_fills(now_ns);
            }
            break;
        }

        case SimEventKind::ExchangeCancelArrival: {
            const CancelAck ack = exchange.on_cancel(now_ns, se.cancel_req);
            if (ack.cancelled && cfg_.fees.cancel_fee_dollars != 0.0) {
                exch_portfolio.on_cancel_fee(cfg_.fees.cancel_fee_dollars);
            }

            ScheduledEvent ack_ev{};
            ack_ev.timestamp_ns = now_ns + cfg_.latency.ack_latency_ns;
            ack_ev.priority = 2;
            ack_ev.seq = sched_seq++;
            ack_ev.kind = SimEventKind::StrategyCancelAck;
            ack_ev.cancel_ack = ack;
            queue.push(ack_ev);
            break;
        }

        case SimEventKind::MarketStrategyEvent: {
            const MarketEvent& ev = se.market_ev;
            if (ev.type == MarketEventType::BookClear) {
                strategy_book->clear();
            } else if (ev.type == MarketEventType::BookDelta) {
                strategy_book->apply_delta(ev.side, ev.price_ticks, ev.qty);
            }

            const BookSignals sig = features.on_event(ev, *strategy_book);
            if (!sig.valid) {
                break;
            }
            const double strat_mark = sig.mid_ticks * cfg_.tick_size_dollars;
            const bool prev_kill = risk.kill_switch_active();
            const bool now_kill =
                risk.update_equity(now_ns, strat_portfolio.net_equity_dollars(strat_mark));
            if (now_kill) {
                if (!prev_kill) {
                    cancel_all_open(now_ns);
                }
                break;
            }

            StrategyState st{};
            st.position = strat_portfolio.position();
            st.max_abs_position = cfg_.risk.max_abs_position;
            st.maker_fee_bps = cfg_.fees.maker_fee_bps;

            const QuoteDecision q = strategy->compute_quotes(sig, st);
            manage_side_quote(now_ns, Side::Bid, q.has_bid, q.bid_price_ticks, q.bid_qty, strat_mark);
            manage_side_quote(now_ns, Side::Ask, q.has_ask, q.ask_price_ticks, q.ask_qty, strat_mark);
            break;
        }

        case SimEventKind::StrategyOrderAck: {
            auto it = oms_orders.find(se.order_ack.order_id);
            if (it == oms_orders.end()) {
                break;
            }
            if (se.order_ack.accepted) {
                ++result.summary.quotes_accepted;
                if (it->second.status == OrderStatus::PendingNew) {
                    it->second.status = OrderStatus::Live;
                    if (risk.kill_switch_active()) {
                        send_cancel(now_ns, it->first);
                    }
                }
            } else {
                ++result.summary.quotes_exch_rejected;
                if (active_bid_id == it->first) {
                    active_bid_id = 0;
                }
                if (active_ask_id == it->first) {
                    active_ask_id = 0;
                }
                oms_orders.erase(it);
            }
            break;
        }

        case SimEventKind::StrategyCancelAck: {
            if (se.cancel_ack.cancelled) {
                ++result.summary.cancels_succeeded;
                if (cfg_.fees.cancel_fee_dollars != 0.0) {
                    strat_portfolio.on_cancel_fee(cfg_.fees.cancel_fee_dollars);
                }
            } else {
                ++result.summary.cancels_too_late;
            }
            auto it = oms_orders.find(se.cancel_ack.order_id);
            if (it != oms_orders.end() && se.cancel_ack.cancelled) {
                if (active_bid_id == it->first) {
                    active_bid_id = 0;
                }
                if (active_ask_id == it->first) {
                    active_ask_id = 0;
                }
                oms_orders.erase(it);
            }
            break;
        }

        case SimEventKind::StrategyFillArrival: {
            strat_portfolio.on_fill(se.fill_ev);
            auto it = oms_orders.find(se.fill_ev.order_id);
            if (it != oms_orders.end()) {
                it->second.remaining_qty =
                    std::max<Qty>(0, it->second.remaining_qty - se.fill_ev.fill_qty);
                if (it->second.remaining_qty == 0) {
                    if (active_bid_id == it->first) {
                        active_bid_id = 0;
                    }
                    if (active_ask_id == it->first) {
                        active_ask_id = 0;
                    }
                    oms_orders.erase(it);
                }
            }
            if (features.latest().valid) {
                const double strat_mark = features.latest().mid_ticks * cfg_.tick_size_dollars;
                const bool prev_kill = risk.kill_switch_active();
                if (risk.update_equity(now_ns, strat_portfolio.net_equity_dollars(strat_mark)) &&
                    !prev_kill) {
                    cancel_all_open(now_ns);
                }
            }
            break;
        }
        }
    }

    accumulate_inventory(end_ns);
    maybe_record_equity(end_ns, true);

    // Populate summary metrics from ground-truth exchange portfolio & post-trade markouts.
    const double final_mark =
        have_exch_mid ? (last_exch_mid_ticks * cfg_.tick_size_dollars) : 0.0;
    result.summary.final_mark_price_dollars = final_mark;
    result.summary.fill_count = all_fills.size();
    result.summary.maker_fills = exchange.stats().maker_fills;
    result.summary.taker_fills = exchange.stats().taker_fills;
    result.summary.total_filled_shares = exch_portfolio.total_traded_shares();
    result.summary.order_fill_rate =
        (result.summary.quotes_accepted > 0)
            ? (static_cast<double>(result.summary.fill_count) /
               static_cast<double>(result.summary.quotes_accepted))
            : 0.0;
    result.summary.share_fill_rate =
        (result.summary.total_quoted_shares > 0)
            ? (static_cast<double>(result.summary.total_filled_shares) /
               static_cast<double>(result.summary.total_quoted_shares))
            : 0.0;

    result.summary.final_position = exch_portfolio.position();
    result.summary.max_abs_position = exch_portfolio.max_abs_position_observed();
    const double total_dt = static_cast<double>(std::max<TimestampNs>(1, end_ns - start_ns));
    result.summary.mean_abs_position = weighted_abs_pos / total_dt;
    result.summary.rms_position = std::sqrt(weighted_sq_pos / total_dt);

    result.summary.cash_dollars = exch_portfolio.cash_dollars();
    result.summary.realized_pnl_dollars = exch_portfolio.realized_pnl_dollars();
    result.summary.unrealized_pnl_dollars = exch_portfolio.unrealized_pnl_dollars(final_mark);
    result.summary.gross_mtm_pnl_dollars = exch_portfolio.gross_mtm_pnl_dollars(final_mark);
    result.summary.total_fees_dollars = exch_portfolio.total_fees_dollars();
    result.summary.net_mtm_pnl_dollars = exch_portfolio.net_mtm_pnl_dollars(final_mark);
    result.summary.max_drawdown_dollars = exch_portfolio.max_drawdown_dollars();
    result.summary.reconciliation_error_dollars =
        exch_portfolio.reconciliation_error_dollars(final_mark);
    result.summary.gross_traded_notional_dollars = exch_portfolio.gross_traded_notional_dollars();
    result.summary.drawdown_kill_triggered = risk.kill_switch_active();
    result.summary.risk_stats = risk.stats();
    result.summary.exchange_stats = exchange.stats();

    // Average captured half-spread across all fills.
    double sum_cap_ticks = 0.0;
    double sum_cap_bps = 0.0;
    Qty sum_cap_qty = 0;
    for (const auto& f : all_fills) {
        const double sgn = static_cast<double>(side_sign(f.side));
        const double cap = sgn * (f.mid_at_fill_ticks - static_cast<double>(f.fill_price_ticks));
        const double w = static_cast<double>(f.fill_qty);
        sum_cap_ticks += w * cap;
        if (f.mid_at_fill_ticks > 0.0) {
            sum_cap_bps += w * cap * 10'000.0 / f.mid_at_fill_ticks;
        }
        sum_cap_qty += f.fill_qty;
    }
    if (sum_cap_qty > 0) {
        result.summary.mean_captured_half_spread_ticks =
            sum_cap_ticks / static_cast<double>(sum_cap_qty);
        result.summary.mean_captured_half_spread_bps =
            sum_cap_bps / static_cast<double>(sum_cap_qty);
    }

    // Adverse-selection markouts at 10 ms / 100 ms / 1 s (strictly where timestamps support it).
    if (record_timeseries) {
        const std::vector<TimestampNs> horizons = {10 * kNsPerMs, 100 * kNsPerMs, 1 * kNsPerSec};
        result.summary.markouts =
            compute_markouts(all_fills, mid_timeline, end_ns, horizons, &result.fills);
    }

    return result;
}

bool write_summary_json(const std::string& path, const SimSummary& s, const SimConfig& cfg,
                        std::string& err) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to open summary JSON: " + path;
        return false;
    }
    ofs << std::fixed << std::setprecision(6);
    ofs << "{\n";
    ofs << "  \"strategy\": \"" << s.strategy_name << "\",\n";
    ofs << "  \"fill_model\": \"" << s.fill_model_name << "\",\n";
    ofs << "  \"book_impl\": \"" << s.book_impl_name << "\",\n";
    ofs << "  \"tick_size_dollars\": " << cfg.tick_size_dollars << ",\n";
    ofs << "  \"md_latency_ns\": " << cfg.latency.md_latency_ns << ",\n";
    ofs << "  \"order_latency_ns\": " << cfg.latency.order_latency_ns << ",\n";
    ofs << "  \"ack_latency_ns\": " << cfg.latency.ack_latency_ns << ",\n";
    ofs << "  \"maker_fee_bps\": " << cfg.fees.maker_fee_bps << ",\n";
    ofs << "  \"taker_fee_bps\": " << cfg.fees.taker_fee_bps << ",\n";
    ofs << "  \"total_market_events\": " << s.total_market_events << ",\n";
    ofs << "  \"session_duration_sec\": " << s.session_duration_sec << ",\n";
    ofs << "  \"quotes_submitted\": " << s.quotes_submitted << ",\n";
    ofs << "  \"quotes_accepted\": " << s.quotes_accepted << ",\n";
    ofs << "  \"quotes_risk_rejected\": " << s.quotes_risk_rejected << ",\n";
    ofs << "  \"quotes_exch_rejected\": " << s.quotes_exch_rejected << ",\n";
    ofs << "  \"cancels_submitted\": " << s.cancels_submitted << ",\n";
    ofs << "  \"cancels_succeeded\": " << s.cancels_succeeded << ",\n";
    ofs << "  \"cancels_too_late\": " << s.cancels_too_late << ",\n";
    ofs << "  \"cancel_races_filled\": " << s.cancel_races_filled << ",\n";
    ofs << "  \"fill_count\": " << s.fill_count << ",\n";
    ofs << "  \"maker_fills\": " << s.maker_fills << ",\n";
    ofs << "  \"taker_fills\": " << s.taker_fills << ",\n";
    ofs << "  \"total_quoted_shares\": " << s.total_quoted_shares << ",\n";
    ofs << "  \"total_filled_shares\": " << s.total_filled_shares << ",\n";
    ofs << "  \"order_fill_rate\": " << s.order_fill_rate << ",\n";
    ofs << "  \"share_fill_rate\": " << s.share_fill_rate << ",\n";
    ofs << "  \"final_position\": " << s.final_position << ",\n";
    ofs << "  \"max_abs_position\": " << s.max_abs_position << ",\n";
    ofs << "  \"mean_abs_position\": " << s.mean_abs_position << ",\n";
    ofs << "  \"rms_position\": " << s.rms_position << ",\n";
    ofs << "  \"final_mark_price_dollars\": " << s.final_mark_price_dollars << ",\n";
    ofs << "  \"cash_dollars\": " << s.cash_dollars << ",\n";
    ofs << "  \"realized_pnl_dollars\": " << s.realized_pnl_dollars << ",\n";
    ofs << "  \"unrealized_pnl_dollars\": " << s.unrealized_pnl_dollars << ",\n";
    ofs << "  \"gross_mtm_pnl_dollars\": " << s.gross_mtm_pnl_dollars << ",\n";
    ofs << "  \"total_fees_dollars\": " << s.total_fees_dollars << ",\n";
    ofs << "  \"net_mtm_pnl_dollars\": " << s.net_mtm_pnl_dollars << ",\n";
    ofs << "  \"max_drawdown_dollars\": " << s.max_drawdown_dollars << ",\n";
    ofs << "  \"reconciliation_error_dollars\": " << std::setprecision(12)
        << s.reconciliation_error_dollars << std::setprecision(6) << ",\n";
    ofs << "  \"gross_traded_notional_dollars\": " << s.gross_traded_notional_dollars << ",\n";
    ofs << "  \"mean_captured_half_spread_ticks\": " << s.mean_captured_half_spread_ticks << ",\n";
    ofs << "  \"mean_captured_half_spread_bps\": " << s.mean_captured_half_spread_bps << ",\n";
    ofs << "  \"drawdown_kill_triggered\": " << (s.drawdown_kill_triggered ? "true" : "false")
        << ",\n";
    ofs << "  \"markouts\": [\n";
    for (size_t i = 0; i < s.markouts.size(); ++i) {
        const auto& m = s.markouts[i];
        ofs << "    {\n";
        ofs << "      \"horizon_ms\": " << (static_cast<double>(m.horizon_ns) / 1e6) << ",\n";
        ofs << "      \"valid_fills\": " << m.valid_fills << ",\n";
        ofs << "      \"valid_shares\": " << m.valid_shares << ",\n";
        ofs << "      \"mean_captured_half_spread_ticks\": " << m.mean_captured_half_spread_ticks
            << ",\n";
        ofs << "      \"mean_adverse_selection_ticks\": " << m.mean_adverse_selection_ticks << ",\n";
        ofs << "      \"mean_realized_spread_ticks\": " << m.mean_realized_spread_ticks << ",\n";
        ofs << "      \"mean_adverse_selection_bps\": " << m.mean_adverse_selection_bps << ",\n";
        ofs << "      \"mean_realized_spread_bps\": " << m.mean_realized_spread_bps << "\n";
        ofs << "    }" << (i + 1 < s.markouts.size() ? "," : "") << "\n";
    }
    ofs << "  ]\n";
    ofs << "}\n";
    if (!ofs) {
        err = "Failed writing summary JSON: " + path;
        return false;
    }
    return true;
}

bool write_fills_csv(const std::string& path, const std::vector<FillMarkoutRow>& fills,
                     double tick_size_dollars, std::string& err) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to open fills CSV: " + path;
        return false;
    }
    ofs << "exchange_time_ns,order_id,side,fill_price_ticks,fill_price_dollars,fill_qty,"
           "liquidity,mid_at_fill_ticks,fee_dollars,raced_cancel,captured_half_spread_ticks,"
           "adv_sel_10ms_ticks,realized_10ms_ticks,adv_sel_100ms_ticks,realized_100ms_ticks,"
           "adv_sel_1s_ticks,realized_1s_ticks\n";
    ofs << std::fixed << std::setprecision(6);
    for (const auto& r : fills) {
        const auto& f = r.fill;
        ofs << f.exchange_time_ns << ',' << f.order_id << ',' << side_to_string(f.side) << ','
            << f.fill_price_ticks << ','
            << (static_cast<double>(f.fill_price_ticks) * tick_size_dollars) << ',' << f.fill_qty
            << ',' << (f.liquidity == LiquidityFlag::Maker ? "MAKER" : "TAKER") << ','
            << f.mid_at_fill_ticks << ',' << f.fee_dollars << ',' << (f.raced_cancel ? 1 : 0) << ','
            << r.captured_half_spread_ticks << ',';
        if (r.has_10ms) {
            ofs << r.adv_sel_10ms_ticks << ',' << r.realized_10ms_ticks << ',';
        } else {
            ofs << ",,";
        }
        if (r.has_100ms) {
            ofs << r.adv_sel_100ms_ticks << ',' << r.realized_100ms_ticks << ',';
        } else {
            ofs << ",,";
        }
        if (r.has_1s) {
            ofs << r.adv_sel_1s_ticks << ',' << r.realized_1s_ticks;
        } else {
            ofs << ",";
        }
        ofs << '\n';
    }
    if (!ofs) {
        err = "Failed writing fills CSV: " + path;
        return false;
    }
    return true;
}

bool write_equity_csv(const std::string& path, const std::vector<EquityPoint>& equity,
                      std::string& err) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to open equity CSV: " + path;
        return false;
    }
    ofs << "timestamp_ns,mid_ticks,position,cash_dollars,realized_pnl_dollars,"
           "unrealized_pnl_dollars,total_fees_dollars,gross_mtm_pnl_dollars,net_mtm_pnl_dollars\n";
    ofs << std::fixed << std::setprecision(6);
    for (const auto& pt : equity) {
        ofs << pt.timestamp_ns << ',' << pt.mid_ticks << ',' << pt.position << ','
            << pt.cash_dollars << ',' << pt.realized_pnl_dollars << ',' << pt.unrealized_pnl_dollars
            << ',' << pt.total_fees_dollars << ',' << pt.gross_mtm_pnl_dollars << ','
            << pt.net_mtm_pnl_dollars << '\n';
    }
    if (!ofs) {
        err = "Failed writing equity CSV: " + path;
        return false;
    }
    return true;
}

} // namespace mmsim
