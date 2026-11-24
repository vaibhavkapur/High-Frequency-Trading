#include "sor/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <queue>
#include <random>
#include <sstream>

#include "sor/consolidated_book.hpp"
#include "sor/oms.hpp"
#include "sor/quote_normalizer.hpp"
#include "sor/risk_manager.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

namespace sor {

namespace {

enum class SimEventType : std::uint8_t {
    MarketTick = 0,
    FeedDelivery = 1,
    ParentArrival = 2,
    ChildArrival = 3,
    ExecReportDelivery = 4,
    VenueOutageToggle = 5,
    FeedFreezeToggle = 6,
};

struct SimEvent {
    TimestampNs ts_ns{0};
    std::uint64_t seq{0};
    SimEventType type{SimEventType::MarketTick};
    VenueId venue_id{kInvalidVenue};
    bool flag{false};
    NormalizedQuoteUpdate quote{};
    ParentOrderRequest parent_req{};
    ChildCommand child_cmd{};
    ExecutionReport exec_rep{};

    bool operator>(const SimEvent& o) const noexcept {
        if (ts_ns != o.ts_ns) return ts_ns > o.ts_ns;
        return seq > o.seq;
    }
};

struct SimVenueRuntime {
    VenueConfig cfg{};
    VenueBook true_book{};
    bool is_down{false};
    bool feed_frozen{false};
    std::uint64_t feed_seq{0};
};

double percentile_sorted(const std::vector<double>& sorted, double p) noexcept {
    if (sorted.empty()) return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

}  // namespace

std::string SimulationMetrics::to_json() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "{"
        << "\"scenario\":\"" << scenario_name << "\","
        << "\"policy\":\"" << policy_name << "\","
        << "\"seed\":" << seed << ","
        << "\"parent_orders\":" << parent_orders << ","
        << "\"filled_orders\":" << filled_orders << ","
        << "\"partial_orders\":" << partial_orders << ","
        << "\"unfilled_orders\":" << unfilled_orders << ","
        << "\"risk_rejected_orders\":" << risk_rejected_orders << ","
        << "\"total_requested_qty\":" << total_requested_qty << ","
        << "\"total_filled_qty\":" << total_filled_qty << ","
        << "\"fill_ratio\":" << fill_ratio << ","
        << "\"gross_is_bps\":" << gross_is_bps << ","
        << "\"fee_cost_bps\":" << fee_cost_bps << ","
        << "\"net_is_bps\":" << net_is_bps << ","
        << "\"total_fees_usd\":" << total_fees_usd << ","
        << "\"median_parent_latency_us\":" << median_parent_latency_us << ","
        << "\"p99_parent_latency_us\":" << p99_parent_latency_us << ","
        << "\"children_sent\":" << children_sent << ","
        << "\"children_filled\":" << children_filled << ","
        << "\"children_cancelled\":" << children_cancelled << ","
        << "\"children_rejected\":" << children_rejected << ","
        << "\"children_timed_out\":" << children_timed_out << ","
        << "\"failovers_triggered\":" << failovers_triggered << ","
        << "\"cancel_to_fill_ratio\":" << cancel_to_fill_ratio << ","
        << "\"venues\":[";
    for (std::size_t i = 0; i < venue_count; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"name\":\"" << venue_names[i] << "\","
            << "\"fill_qty\":" << venue_fill_qty[i] << ","
            << "\"fill_share\":" << venue_fill_share[i] << ","
            << "\"children_sent\":" << venue_child_sent[i] << "}";
    }
    oss << "]}";
    return oss.str();
}

SimulationOutput ExecutionSimulator::run(const SystemConfig& base_cfg) {
    SystemConfig cfg = base_cfg;

    // Apply scenario-specific parameterizations cleanly while preserving user scaling overrides.
    for (auto& v : cfg.venues) {
        v.base_one_way_latency_ns = static_cast<TimestampNs>(
            std::llround(static_cast<double>(v.base_one_way_latency_ns) * cfg.sim.latency_scale));
        v.latency_jitter_ns = static_cast<TimestampNs>(
            std::llround(static_cast<double>(v.latency_jitter_ns) * cfg.sim.latency_scale));
        v.taker_fee_per_share_ticks = static_cast<PriceTicks>(
            std::llround(static_cast<double>(v.taker_fee_per_share_ticks) * cfg.sim.fee_scale));
        if (cfg.sim.stale_timeout_override_ns > 0) {
            v.stale_timeout_ns = cfg.sim.stale_timeout_override_ns;
        }
    }

    // In NaiveBestPrice baseline under StaleFeedTrap, disable staleness gating if caller didn't
    // already differentiate, OR keep RouterConfig::respect_staleness explicit:
    // Actually, NaiveBestPrice by definition in the README is a naïve raw-price-only router
    // (ignores fees, latency, and staleness). Let's apply that in simulation for NaiveBestPrice
    // and NaivePrimary so baseline comparisons reflect realistic naïve routers, while unit tests
    // testing RoutePlanner directly control RouterConfig::respect_staleness explicitly!
    if (cfg.router.policy == RoutingPolicy::NaiveBestPrice ||
        cfg.router.policy == RoutingPolicy::NaivePrimary) {
        cfg.router.respect_staleness = false;
        cfg.router.respect_venue_health = false;
    }

    // Scenario tuning on venue properties when using default 3-venue layout.
    if (cfg.venues.size() >= 3) {
        if (cfg.sim.scenario == ScenarioType::FeeInversion) {
            // Venue 0 (ARCA) expensive taker fee, Venue 1 (BATS_INV) strong taker rebate,
            // Venue 2 (IEX_SIM) low fee. Latencies equalized so fee difference is isolated.
            cfg.venues[0].taker_fee_per_share_ticks =
                static_cast<PriceTicks>(std::llround(35.0 * cfg.sim.fee_scale));
            cfg.venues[1].taker_fee_per_share_ticks =
                static_cast<PriceTicks>(std::llround(-25.0 * cfg.sim.fee_scale));
            cfg.venues[2].taker_fee_per_share_ticks =
                static_cast<PriceTicks>(std::llround(8.0 * cfg.sim.fee_scale));
            for (auto& v : cfg.venues) {
                v.base_one_way_latency_ns =
                    static_cast<TimestampNs>(std::llround(120'000.0 * cfg.sim.latency_scale));
                v.latency_jitter_ns = 10'000;
            }
        } else if (cfg.sim.scenario == ScenarioType::LatencyRaceDepletion) {
            // Equal fees, stark latency asymmetry: Venue 0 fast (80us), Venue 1 medium (450us),
            // Venue 2 slow (1,800us) displaying slightly better top-of-book that often depletes.
            for (auto& v : cfg.venues) {
                v.taker_fee_per_share_ticks =
                    static_cast<PriceTicks>(std::llround(10.0 * cfg.sim.fee_scale));
            }
            cfg.venues[0].base_one_way_latency_ns =
                static_cast<TimestampNs>(std::llround(80'000.0 * cfg.sim.latency_scale));
            cfg.venues[1].base_one_way_latency_ns =
                static_cast<TimestampNs>(std::llround(450'000.0 * cfg.sim.latency_scale));
            cfg.venues[2].base_one_way_latency_ns =
                static_cast<TimestampNs>(std::llround(1'800'000.0 * cfg.sim.latency_scale));
            cfg.venues[2].base_fill_prob = 0.65;
            cfg.venues[2].latency_decay_per_ms = 0.45;
        }
    }

    // Core router components.
    QuoteNormalizer normalizer;
    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    for (const auto& v : cfg.venues) {
        normalizer.register_venue(v.id);
    }

    RoutePlanner planner(cfg.router);
    RiskManager risk(cfg.risk);
    VenueHealthMonitor health(cfg.health);
    AuditLog audit;
    audit.set_enabled(cfg.sim.record_audit_log);
    OrderManagementSystem oms(book, planner, risk, health, audit);

    std::array<SimVenueRuntime, kMaxVenues> venues{};
    std::vector<VenueId> active_vids;
    for (const auto& v : cfg.venues) {
        venues[v.id].cfg = v;
        venues[v.id].true_book.venue_id = v.id;
        active_vids.push_back(v.id);
    }

    std::mt19937_64 market_rng(cfg.sim.seed);
    std::mt19937_64 exec_rng(cfg.sim.seed ^ 0xA5A5A5A5A5A5A5A5ULL);
    std::priority_queue<SimEvent, std::vector<SimEvent>, std::greater<SimEvent>> pq;
    std::uint64_t next_event_seq = 1;
    ExecId next_exec_id = 1;

    auto push_event = [&](SimEvent ev) {
        ev.seq = next_event_seq++;
        pq.push(std::move(ev));
    };

    auto sample_latency_ns = [&](const VenueConfig& vcfg) -> TimestampNs {
        if (vcfg.latency_jitter_ns <= 0) return std::max<TimestampNs>(1'000, vcfg.base_one_way_latency_ns);
        std::uniform_int_distribution<TimestampNs> dist(-vcfg.latency_jitter_ns, vcfg.latency_jitter_ns);
        return std::max<TimestampNs>(1'000, vcfg.base_one_way_latency_ns + dist(exec_rng));
    };

    auto publish_venue_snapshot = [&](VenueId vid, TimestampNs exch_ts) {
        auto& vr = venues[vid];
        if (vr.feed_frozen) return;

        for (Side s : {Side::Buy, Side::Sell}) {
            NormalizedQuoteUpdate u{};
            u.venue_id = vid;
            u.symbol_id = 1;
            u.seq = ++vr.feed_seq;
            u.exchange_ts_ns = exch_ts;
            const TimestampNs feed_lat = std::max<TimestampNs>(10'000, vr.cfg.base_one_way_latency_ns / 2);
            u.recv_ts_ns = exch_ts + feed_lat;
            u.type = QuoteUpdateType::Snapshot;
            u.side = s;
            const auto& arr = (s == Side::Buy) ? vr.true_book.bids : vr.true_book.asks;
            const std::size_t cnt = (s == Side::Buy) ? vr.true_book.bid_count : vr.true_book.ask_count;
            u.level_count = static_cast<std::uint8_t>(std::min(cnt, kMaxBookDepth));
            for (std::size_t i = 0; i < u.level_count; ++i) {
                u.levels[i] = arr[i];
            }
            SimEvent fev{};
            fev.ts_ns = u.recv_ts_ns;
            fev.type = SimEventType::FeedDelivery;
            fev.venue_id = vid;
            fev.quote = u;
            push_event(std::move(fev));
        }
    };

    // Current latent fair mid price (in ticks).
    PriceTicks latent_mid = cfg.sim.initial_mid_ticks;
    const TimestampNs kStartTs = 1'000'000'000;   // t = 1.0 s
    const TimestampNs kOrderSpacingNs = 20'000'000;  // 20 ms between parent orders
    const TimestampNs kMarketTickSpacingNs = 2'000'000;  // 2 ms market quote refresh

    auto rebuild_venue_books = [&](TimestampNs now_ns) {
        std::uniform_int_distribution<Quantity> base_size_dist(150, 600);
        for (VenueId vid : active_vids) {
            auto& vr = venues[vid];
            std::array<PriceLevel, 5> bids{};
            std::array<PriceLevel, 5> asks{};

            // Venue-specific top-of-book offset in ticks (100 ticks = $0.01).
            PriceTicks bid_adj = 0;
            PriceTicks ask_adj = 0;
            double depth_mult = cfg.sim.liquidity_scale;

            if (cfg.sim.scenario == ScenarioType::FeeInversion) {
                // Venue 0 (high fee) frequently shows 1/4 cent tighter raw ask/bid OR tied price,
                // while Venue 1 (rebate) is 0..20 ticks worse raw but better fee-adjusted!
                if (vid == 0) {
                    ask_adj = -15;  // $0.0015 cheaper raw than mid+50, but +$0.0035 fee!
                    bid_adj = +15;
                    depth_mult *= 1.1;
                } else if (vid == 1) {
                    ask_adj = +5;   // $0.0020 worse raw than V0, but -$0.0025 rebate -> $0.0040 net better!
                    bid_adj = -5;
                    depth_mult *= 0.9;
                } else {
                    ask_adj = 0;
                    bid_adj = 0;
                    depth_mult *= 0.8;
                }
            } else if (cfg.sim.scenario == ScenarioType::LatencyRaceDepletion) {
                if (vid == 0) {
                    ask_adj = +10;  // Fast venue slightly wider raw, deep, reliable
                    bid_adj = -10;
                    depth_mult *= 1.3;
                } else if (vid == 1) {
                    ask_adj = 0;
                    bid_adj = 0;
                    depth_mult *= 0.9;
                } else {
                    // Slow venue advertises tempting thin top-of-book that competing flow picks off!
                    ask_adj = -20;
                    bid_adj = +20;
                    depth_mult *= 0.45;
                }
            } else if (cfg.sim.scenario == ScenarioType::StaleFeedTrap) {
                // When Venue 1's feed is frozen, its true book moves with latent_mid while router
                // still sees old frozen quote!
                if (vid == 1 && !vr.feed_frozen) {
                    ask_adj = -25;
                    bid_adj = +25;
                }
            } else {
                // MixedStress & VenueFailover: mild venue dispersion.
                std::uniform_int_distribution<int> jit(-1, 1);
                ask_adj = jit(market_rng) * 15;
                bid_adj = jit(market_rng) * 15;
                if (vid == 0) depth_mult *= 1.2;
                if (vid == 2) depth_mult *= 0.75;
            }

            const PriceTicks base_bid = latent_mid - 50 + bid_adj;
            const PriceTicks base_ask = latent_mid + 50 + ask_adj;

            for (std::size_t lvl = 0; lvl < 5; ++lvl) {
                const Quantity raw_q = base_size_dist(market_rng);
                const Quantity q =
                    std::max<Quantity>(50, static_cast<Quantity>(std::llround(
                                               static_cast<double>(raw_q) * depth_mult *
                                               (1.0 + 0.35 * static_cast<double>(lvl)))));
                bids[lvl] = PriceLevel{base_bid - static_cast<PriceTicks>(lvl) * 100, q, 2};
                asks[lvl] = PriceLevel{base_ask + static_cast<PriceTicks>(lvl) * 100, q, 2};
            }
            vr.true_book.set_snapshot(Side::Buy, bids.data(), bids.size());
            vr.true_book.set_snapshot(Side::Sell, asks.data(), asks.size());
            publish_venue_snapshot(vid, now_ns);
        }
    };

    // Initial book state before first parent order.
    rebuild_venue_books(kStartTs - 5'000'000);

    // Schedule market ticks across the full simulation horizon.
    const TimestampNs total_duration =
        static_cast<TimestampNs>(cfg.sim.num_parent_orders + 2) * kOrderSpacingNs;
    for (TimestampNs t = kStartTs; t <= kStartTs + total_duration; t += kMarketTickSpacingNs) {
        SimEvent mev{};
        mev.ts_ns = t;
        mev.type = SimEventType::MarketTick;
        push_event(std::move(mev));
    }

    // Schedule scenario-specific failure / staleness windows.
    if (cfg.sim.scenario == ScenarioType::StaleFeedTrap && active_vids.size() >= 2) {
        // Freeze Venue 1 feed during 3 windows while drifting prices away from frozen quotes!
        for (int w = 0; w < 3; ++w) {
            const TimestampNs start = kStartTs + (total_duration / 6) * (2 * w + 1) - 10'000'000;
            const TimestampNs end = start + (total_duration / 5);
            SimEvent f1{};
            f1.ts_ns = start;
            f1.type = SimEventType::FeedFreezeToggle;
            f1.venue_id = 1;
            f1.flag = true;
            push_event(std::move(f1));

            SimEvent f2{};
            f2.ts_ns = end;
            f2.type = SimEventType::FeedFreezeToggle;
            f2.venue_id = 1;
            f2.flag = false;
            push_event(std::move(f2));
        }
    } else if (cfg.sim.scenario == ScenarioType::VenueFailover && active_vids.size() >= 2) {
        const TimestampNs down_start = kStartTs + total_duration / 4;
        const TimestampNs down_end = kStartTs + (3 * total_duration) / 4;
        SimEvent d1{};
        d1.ts_ns = down_start;
        d1.type = SimEventType::VenueOutageToggle;
        d1.venue_id = 0;
        d1.flag = true;
        push_event(std::move(d1));

        SimEvent d2{};
        d2.ts_ns = down_end;
        d2.type = SimEventType::VenueOutageToggle;
        d2.venue_id = 0;
        d2.flag = false;
        push_event(std::move(d2));
    } else if (cfg.sim.scenario == ScenarioType::MixedStress && active_vids.size() >= 3) {
        // Both a feed freeze on Venue 2 and an outage window on Venue 0.
        SimEvent f1{};
        f1.ts_ns = kStartTs + total_duration / 5;
        f1.type = SimEventType::FeedFreezeToggle;
        f1.venue_id = 2;
        f1.flag = true;
        push_event(std::move(f1));

        SimEvent f2{};
        f2.ts_ns = kStartTs + (2 * total_duration) / 5;
        f2.type = SimEventType::FeedFreezeToggle;
        f2.venue_id = 2;
        f2.flag = false;
        push_event(std::move(f2));

        SimEvent d1{};
        d1.ts_ns = kStartTs + (3 * total_duration) / 5;
        d1.type = SimEventType::VenueOutageToggle;
        d1.venue_id = 0;
        d1.flag = true;
        push_event(std::move(d1));

        SimEvent d2{};
        d2.ts_ns = kStartTs + (4 * total_duration) / 5;
        d2.type = SimEventType::VenueOutageToggle;
        d2.venue_id = 0;
        d2.flag = false;
        push_event(std::move(d2));
    }

    // Schedule parent order arrivals using deterministic seeded PRNG (separate stream so
    // parent order sequence is identical across policies!).
    std::mt19937_64 order_rng(cfg.sim.seed ^ 0x9E3779B97F4A7C15ULL);
    std::uniform_int_distribution<Quantity> qty_dist(cfg.sim.min_parent_qty,
                                                     cfg.sim.max_parent_qty);
    std::uniform_int_distribution<int> side_dist(0, 1);

    for (std::uint32_t i = 0; i < cfg.sim.num_parent_orders; ++i) {
        SimEvent pev{};
        pev.ts_ns = kStartTs + 5'000'000 + static_cast<TimestampNs>(i) * kOrderSpacingNs;
        pev.type = SimEventType::ParentArrival;
        pev.parent_req.client_order_id = "ORD-" + std::to_string(i + 1);
        pev.parent_req.symbol_id = 1;
        pev.parent_req.side = (side_dist(order_rng) == 0) ? Side::Buy : Side::Sell;
        pev.parent_req.quantity = qty_dist(order_rng);
        pev.parent_req.tif = TimeInForce::IOC;
        pev.parent_req.timestamp_ns = pev.ts_ns;
        // Limit price set dynamically at arrival relative to mid (+/- 35 bps marketable collar)
        // when limit_price == 0.
        pev.parent_req.limit_price = 0;
        push_event(std::move(pev));
    }

    auto dispatch_commands = [&](const std::vector<ChildCommand>& cmds) {
        for (const auto& cmd : cmds) {
            if (cmd.venue_id >= kMaxVenues) continue;
            const TimestampNs lat = sample_latency_ns(venues[cmd.venue_id].cfg);
            SimEvent cev{};
            cev.ts_ns = cmd.timestamp_ns + lat;
            cev.type = SimEventType::ChildArrival;
            cev.venue_id = cmd.venue_id;
            cev.child_cmd = cmd;
            push_event(std::move(cev));
        }
    };

    std::uniform_int_distribution<int> walk_dist(-2, 2);
    std::uniform_real_distribution<double> unit_dist(0.0, 1.0);
    std::vector<ChildCommand> out_cmds;

    while (!pq.empty()) {
        SimEvent ev = pq.top();
        pq.pop();
        out_cmds.clear();

        switch (ev.type) {
            case SimEventType::MarketTick: {
                // Random walk of latent fair mid (uses decoupled market_rng).
                PriceTicks step = static_cast<PriceTicks>(walk_dist(market_rng)) * 50;
                if (cfg.sim.scenario == ScenarioType::StaleFeedTrap && active_vids.size() >= 2 &&
                    venues[1].feed_frozen) {
                    // Larger bidirectional volatility while Venue 1 feed is frozen so the stale
                    // quote diverges significantly from the live market on both buys and sells.
                    step = (step >= 0 ? 1 : -1) * 150;
                }
                latent_mid = std::max<PriceTicks>(100'000, latent_mid + step);
                rebuild_venue_books(ev.ts_ns);
                break;
            }

            case SimEventType::FeedDelivery: {
                NormalizedQuoteUpdate norm{};
                if (normalizer.normalize(ev.quote, norm)) {
                    book.apply_update(norm);
                }
                break;
            }

            case SimEventType::FeedFreezeToggle: {
                if (ev.venue_id < kMaxVenues) {
                    venues[ev.venue_id].feed_frozen = ev.flag;
                    if (!ev.flag) {
                        publish_venue_snapshot(ev.venue_id, ev.ts_ns);
                    }
                }
                break;
            }

            case SimEventType::VenueOutageToggle: {
                if (ev.venue_id < kMaxVenues) {
                    venues[ev.venue_id].is_down = ev.flag;
                    if (!ev.flag) {
                        health.set_manual_down(ev.venue_id, false, ev.ts_ns);
                    }
                }
                break;
            }

            case SimEventType::ParentArrival: {
                ParentOrderRequest req = ev.parent_req;
                // True arrival mid (across live venue books) for apples-to-apples IS across policies.
                double true_mid = static_cast<double>(latent_mid);
                const auto bbo = book.consolidated_bbo(ev.ts_ns, &health, true, true);
                const double ref_mid = bbo.mid_price_ticks() > 0.0 ? bbo.mid_price_ticks() : true_mid;
                req.benchmark_mid_ticks = true_mid;
                // Marketable limit order with +/- 40 bps protective collar around mid.
                const PriceTicks collar =
                    static_cast<PriceTicks>(std::llround(ref_mid * 40.0 / 10'000.0));
                if (req.limit_price <= 0) {
                    req.limit_price = (req.side == Side::Buy)
                                          ? (static_cast<PriceTicks>(std::llround(ref_mid)) + collar)
                                          : std::max<PriceTicks>(
                                                100, static_cast<PriceTicks>(std::llround(ref_mid)) - collar);
                }
                oms.submit_parent(req, out_cmds);
                dispatch_commands(out_cmds);
                break;
            }

            case SimEventType::ChildArrival: {
                const auto& cmd = ev.child_cmd;
                if (cmd.venue_id >= kMaxVenues) break;
                auto& vr = venues[cmd.venue_id];
                const TimestampNs ret_lat = sample_latency_ns(vr.cfg);

                auto send_report = [&](ExecutionReport rep, TimestampNs extra_delay = 0) {
                    rep.child_id = cmd.child_id;
                    rep.venue_id = cmd.venue_id;
                    rep.timestamp_ns = ev.ts_ns + ret_lat + extra_delay;
                    SimEvent rev{};
                    rev.ts_ns = rep.timestamp_ns;
                    rev.type = SimEventType::ExecReportDelivery;
                    rev.venue_id = cmd.venue_id;
                    rev.exec_rep = std::move(rep);
                    push_event(std::move(rev));
                };

                if (cmd.type == ChildCommandType::CancelOrder) {
                    ExecutionReport rep{};
                    rep.type = ExecReportType::CancelAck;
                    send_report(std::move(rep));
                    break;
                }
                if (cmd.type == ChildCommandType::ReplaceOrder) {
                    ExecutionReport rep{};
                    rep.type = ExecReportType::ReplaceAck;
                    send_report(std::move(rep));
                    break;
                }

                // NewOrder handling on simulated venue matching engine.
                if (vr.is_down) {
                    ExecutionReport rep{};
                    rep.type = ExecReportType::Reject;
                    rep.reason = "venue_outage_reject";
                    send_report(std::move(rep));
                    break;
                }

                // Send Ack first.
                {
                    ExecutionReport ack{};
                    ack.type = ExecReportType::Ack;
                    send_report(std::move(ack));
                }

                // Simulate in-flight queue depletion by competing market flow!
                // Longer one-way flight time increases probability that top-of-book liquidity
                // is partially or fully consumed before our child arrives.
                const double flight_ms =
                    static_cast<double>(ev.ts_ns - cmd.timestamp_ns) / 1'000'000.0;
                const double deplete_prob =
                    1.0 - std::exp(-cfg.sim.depletion_rate * std::max(0.0, flight_ms) *
                                   (vr.cfg.latency_decay_per_ms / 0.20));
                const Side contra = (cmd.side == Side::Buy) ? Side::Sell : Side::Buy;
                auto& levels = (contra == Side::Buy) ? vr.true_book.bids : vr.true_book.asks;
                auto& level_cnt =
                    (contra == Side::Buy) ? vr.true_book.bid_count : vr.true_book.ask_count;

                if (level_cnt > 0 && unit_dist(exec_rng) < deplete_prob) {
                    // Competing flow consumes 50%-100% of top level during flight AND shifts
                    // remaining venue levels adversely by 1 tick (informed flow / adverse selection).
                    const double frac = 0.5 + 0.5 * unit_dist(exec_rng);
                    const Quantity consumed = static_cast<Quantity>(
                        std::llround(static_cast<double>(levels[0].quantity) * frac));
                    if (consumed >= levels[0].quantity) {
                        for (std::size_t j = 1; j < level_cnt; ++j) {
                            levels[j - 1] = levels[j];
                        }
                        --level_cnt;
                    } else {
                        levels[0].quantity -= consumed;
                    }
                    const PriceTicks adverse_shift = (cmd.side == Side::Buy) ? 100 : -100;
                    for (std::size_t j = 0; j < level_cnt; ++j) {
                        levels[j].price = std::max<PriceTicks>(100, levels[j].price + adverse_shift);
                    }
                }

                // Match child order against true venue book up to child limit price.
                Quantity rem = cmd.quantity;
                TimestampNs fill_seq_delay = 1'000;
                std::size_t idx = 0;
                while (rem > 0 && idx < level_cnt) {
                    const auto& lvl = levels[idx];
                    const bool marketable = (cmd.side == Side::Buy)
                                                ? (lvl.price <= cmd.limit_price)
                                                : (lvl.price >= cmd.limit_price);
                    if (!marketable) break;

                    const Quantity exec_qty = std::min(rem, lvl.quantity);
                    if (exec_qty > 0) {
                        ExecutionReport fill{};
                        fill.type = ExecReportType::Fill;
                        fill.exec_id = next_exec_id++;
                        fill.fill_qty = exec_qty;
                        fill.fill_price = lvl.price;
                        fill.fee_usd = vr.cfg.taker_fee_usd(lvl.price, exec_qty);
                        send_report(std::move(fill), fill_seq_delay);
                        fill_seq_delay += 500;

                        rem -= exec_qty;
                        levels[idx].quantity -= exec_qty;
                    }
                    if (levels[idx].quantity <= 0) {
                        for (std::size_t j = idx + 1; j < level_cnt; ++j) {
                            levels[j - 1] = levels[j];
                        }
                        --level_cnt;
                    } else {
                        ++idx;
                    }
                }

                // IOC residual cancel ack if not fully filled.
                if (rem > 0) {
                    ExecutionReport cxl{};
                    cxl.type = ExecReportType::CancelAck;
                    cxl.reason = "ioc_unfilled_remainder";
                    send_report(std::move(cxl), fill_seq_delay + 500);
                }

                publish_venue_snapshot(cmd.venue_id, ev.ts_ns);
                break;
            }

            case SimEventType::ExecReportDelivery: {
                oms.on_execution_report(ev.exec_rep, out_cmds);
                dispatch_commands(out_cmds);
                break;
            }
        }
    }

    // Aggregate simulation metrics.
    SimulationOutput output{};
    auto& m = output.metrics;
    m.scenario_name = std::string(to_string(cfg.sim.scenario));
    m.policy_name = std::string(to_string(cfg.router.policy));
    m.seed = cfg.sim.seed;
    m.venue_count = cfg.venues.size();
    for (std::size_t i = 0; i < cfg.venues.size(); ++i) {
        m.venue_names[i] = cfg.venues[i].name;
    }

    double weighted_gross_is = 0.0;
    double weighted_net_is = 0.0;
    double weighted_fee_bps = 0.0;
    std::vector<double> parent_latencies_us;
    parent_latencies_us.reserve(oms.parents().size());

    for (const auto& [pid, p] : oms.parents()) {
        ++m.parent_orders;
        m.total_requested_qty += p.orig_qty;
        if (p.state == ParentOrderState::Rejected) {
            ++m.risk_rejected_orders;
            continue;
        }
        m.total_filled_qty += p.filled_qty;
        m.total_fees_usd += p.total_fees_usd;
        if (p.filled_qty == p.orig_qty) {
            ++m.filled_orders;
        } else if (p.filled_qty > 0) {
            ++m.partial_orders;
        } else {
            ++m.unfilled_orders;
        }

        if (p.filled_qty > 0 && p.arrival_mid_ticks > 0.0) {
            const double w = static_cast<double>(p.filled_qty);
            const double gross = p.gross_shortfall_bps();
            const double net = p.net_shortfall_bps();
            weighted_gross_is += w * gross;
            weighted_net_is += w * net;
            weighted_fee_bps += w * (net - gross);
        }
        if (p.completion_ts_ns > p.arrival_ts_ns) {
            parent_latencies_us.push_back(
                static_cast<double>(p.completion_ts_ns - p.arrival_ts_ns) / 1'000.0);
        }
    }

    for (const auto& [cid, ch] : oms.children()) {
        for (std::size_t i = 0; i < cfg.venues.size(); ++i) {
            if (cfg.venues[i].id == ch.venue_id) {
                m.venue_fill_qty[i] += ch.filled_qty;
                ++m.venue_child_sent[i];
                break;
            }
        }
    }

    m.fill_ratio = m.total_requested_qty > 0
                       ? (static_cast<double>(m.total_filled_qty) /
                          static_cast<double>(m.total_requested_qty))
                       : 0.0;
    if (m.total_filled_qty > 0) {
        const double denom = static_cast<double>(m.total_filled_qty);
        m.gross_is_bps = weighted_gross_is / denom;
        m.net_is_bps = weighted_net_is / denom;
        m.fee_cost_bps = weighted_fee_bps / denom;
        for (std::size_t i = 0; i < m.venue_count; ++i) {
            m.venue_fill_share[i] = static_cast<double>(m.venue_fill_qty[i]) / denom;
        }
    }

    std::sort(parent_latencies_us.begin(), parent_latencies_us.end());
    m.median_parent_latency_us = percentile_sorted(parent_latencies_us, 0.50);
    m.p99_parent_latency_us = percentile_sorted(parent_latencies_us, 0.99);

    const auto& st = oms.stats();
    m.children_sent = st.children_sent;
    m.children_filled = st.children_filled;
    m.children_cancelled = st.children_cancelled;
    m.children_rejected = st.children_rejected;
    m.children_timed_out = st.children_timed_out;
    m.failovers_triggered = st.failovers_triggered;
    m.cancel_to_fill_ratio =
        (st.children_filled > 0)
            ? (static_cast<double>(st.children_cancelled) / static_cast<double>(st.children_filled))
            : static_cast<double>(st.children_cancelled);

    output.audit_log = std::move(audit);
    return output;
}

}  // namespace sor
