// Property / fuzz tests. Deterministic (seeded) so failures reproduce.
// Run them under -DHFTBT_SANITIZE=ON to turn memory errors into failures.
#include <gtest/gtest.h>

#include <unordered_map>

#include "hftbt/rng.hpp"
#include "hftbt/strategies.hpp"
#include "hftbt/synth.hpp"
#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

TEST(Fuzz, BinaryParserSurvivesRandomAndMutatedInput) {
    Rng rng(1234);
    const std::vector<std::byte> good =
        to_bin(generate_l3({.seed = 1, .events = 200, .instruments = 3}));
    std::size_t accepted = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        std::vector<std::byte> buf;
        if (iter % 4 == 0) { // pure noise, sometimes with a valid magic
            buf.resize(rng.below(200));
            for (auto& b : buf) b = static_cast<std::byte>(rng.below(256));
            if (iter % 8 == 0 && buf.size() >= 8) std::copy_n(good.begin(), 8, buf.begin());
        } else { // valid file with a few corruptions / truncation / extension
            buf = good;
            const auto flips = 1 + rng.below(4);
            for (std::uint64_t k = 0; k < flips; ++k)
                buf[rng.below(buf.size())] = static_cast<std::byte>(rng.below(256));
            if (rng.chance(20)) buf.resize(rng.below(buf.size() + 1));
            if (rng.chance(10)) buf.resize(buf.size() + rng.below(80), std::byte{0});
        }
        FeedData out;
        std::string err;
        if (parse_bin({buf.data(), buf.size()}, out, err)) {
            ++accepted;
            // Anything accepted must be safe to hand to the engine.
            std::string verr;
            ASSERT_TRUE(validate_feed(out, verr)) << verr;
            NoopStrategy noop;
            Engine engine(out, {}, noop);
            engine.run();
        } else {
            ASSERT_FALSE(err.empty());
            ASSERT_TRUE(out.events.empty());
        }
    }
    EXPECT_GT(accepted, 0u); // mutations of payload bytes often stay valid
}

TEST(Fuzz, CsvParserSurvivesMutatedInput) {
    Rng rng(99);
    const std::string good = to_csv(generate_l3({.seed = 2, .events = 60, .instruments = 2}));
    const char alphabet[] = "0123456789,,\n-ABCDELTSX. \r#";
    for (int iter = 0; iter < 20000; ++iter) {
        std::string text = good;
        const auto edits = 1 + rng.below(6);
        for (std::uint64_t k = 0; k < edits; ++k) {
            const std::size_t at = rng.below(text.size());
            const char c = alphabet[rng.below(sizeof(alphabet) - 1)];
            if (rng.chance(50))
                text[at] = c;
            else if (rng.chance(50))
                text.insert(at, 1, c);
            else
                text.erase(at, 1 + rng.below(10));
        }
        FeedData out;
        std::string err;
        if (parse_csv(text, out, err)) {
            std::string verr;
            ASSERT_TRUE(validate_feed(out, verr)) << verr;
        } else {
            ASSERT_FALSE(err.empty());
        }
    }
}

namespace {

// Structurally valid but semantically hostile feed: unknown ids, duplicate
// ids, crossed books, L2 and L3 mixed on one instrument, huge sizes.
FeedData hostile_feed(std::uint64_t seed, std::size_t n) {
    Rng rng(seed);
    FeedData f = feed_of({}, 2);
    Timestamp ts = 1000;
    const MdType types[] = {MdType::Add,     MdType::Add,   MdType::Cancel, MdType::Delete,
                            MdType::Execute, MdType::Level, MdType::Trade};
    for (std::size_t i = 0; i < n; ++i) {
        ts += rng.range(0, 50);
        MarketEvent e;
        e.ts = ts;
        e.type = types[rng.below(std::size(types))];
        e.instrument = static_cast<InstrumentId>(rng.below(2));
        e.side = rng.chance(50) ? Side::Buy : Side::Sell;
        e.order_id = rng.below(40); // collisions and dangling references on purpose
        e.price = rng.chance(2) ? 0 : rng.range(90, 110);
        e.qty = rng.chance(2) ? 1'000'000'000 : rng.range(0, 30);
        f.events.push_back(e);
    }
    return f;
}

// Does something random on every callback and checks what it is told.
struct Chaos final : Strategy {
    explicit Chaos(std::uint64_t seed, const LatencyConfig& lat) : rng(seed), lat(lat) {}
    Rng rng;
    LatencyConfig lat;
    struct Mine {
        Timestamp sent;
        Qty qty;
        Qty filled = 0;
        bool done = false;
    };
    std::unordered_map<OrderId, Mine> mine;
    std::vector<OrderId> ids;
    Timestamp last_now = 0;

    void act(Context& c) {
        ASSERT_GE(c.now(), last_now);
        last_now = c.now();
        const auto i = static_cast<InstrumentId>(rng.below(c.num_instruments()));
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        const std::uint64_t roll = rng.below(100);
        SendResult r;
        Qty qty = rng.range(1, 25);
        if (roll < 25)
            r = c.send_limit(i, side, rng.range(88, 112), qty, Tif::GTC, rng.chance(20));
        else if (roll < 35)
            r = c.send_limit(i, side, rng.range(88, 112), qty, Tif::IOC);
        else if (roll < 40)
            r = c.send_market(i, side, qty);
        else if (roll < 60 && !ids.empty())
            c.cancel(ids[rng.below(ids.size())]);
        if (r) {
            mine.emplace(r.id, Mine{c.now(), qty});
            ids.push_back(r.id);
        }
    }
    void on_market_data(Context& c, const MarketEvent& e) override {
        ASSERT_GE(c.now(), e.ts + lat.md_delay_ns);
        act(c);
    }
    void on_order_update(Context& c, const ExecReport& r) override {
        const auto it = mine.find(r.order_id);
        ASSERT_NE(it, mine.end()) << "report for an order we never sent";
        Mine& m = it->second;
        // Nothing can happen at the exchange before the order got there, and
        // nothing can be known here before the response came back.
        ASSERT_GE(r.exch_ts, m.sent + lat.compute_delay_ns + lat.order_delay_ns);
        ASSERT_GE(c.now(), r.exch_ts + lat.response_delay_ns);
        if (r.type == ExecType::Fill) {
            ASSERT_FALSE(m.done) << "fill after a terminal report";
            ASSERT_GT(r.qty, 0);
            ASSERT_GT(r.price, 0);
            m.filled += r.qty;
            ASSERT_LE(m.filled, m.qty) << "over-fill";
            ASSERT_EQ(r.leaves, m.qty - m.filled);
            if (r.leaves == 0) m.done = true;
        } else if (r.type == ExecType::Cancelled || r.type == ExecType::Rejected) {
            ASSERT_FALSE(m.done);
            m.done = true;
        }
        act(c);
    }
};

} // namespace

TEST(Fuzz, EngineInvariantsHoldOnHostileFeedsWithChaoticStrategy) {
    Rng pick(555);
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        const FeedData hostile = hostile_feed(seed, 4000);
        const FeedData clean = generate_l3({.seed = seed,
                                            .events = 4000,
                                            .instruments = 2,
                                            .initial_mid = 100,
                                            .mean_gap_ns = 30});
        const FeedData clean_l2 = l3_to_l2(clean);
        for (const FeedData* feed : {&hostile, &clean, &clean_l2}) {
            EngineConfig cfg;
            cfg.seed = seed;
            cfg.latency.md_delay_ns = pick.range(0, 200);
            cfg.latency.md_jitter_ns = pick.range(0, 100);
            cfg.latency.compute_delay_ns = pick.range(0, 50);
            cfg.latency.order_delay_ns = pick.range(0, 300);
            cfg.latency.order_jitter_ns = pick.range(0, 100);
            cfg.latency.response_delay_ns = pick.range(0, 300);
            cfg.latency.response_jitter_ns = pick.range(0, 100);
            cfg.exchange.queue_model = static_cast<QueueModel>(pick.below(3));
            cfg.exchange.fill_on_crossing_quote = pick.chance(50);
            cfg.exchange.taker_slippage_ticks = pick.range(0, 2);
            cfg.fees.maker_bps = -0.5;
            cfg.fees.taker_bps = 1.0;
            cfg.risk.max_position = 60;
            cfg.risk.max_open_orders = 30;
            cfg.sample_interval_ns = 500;
            cfg.markout_horizons_ns = {100, 1000};
            cfg.md_fifo = pick.chance(50);

            Chaos chaos(seed * 31, cfg.latency);
            Engine engine(*feed, cfg, chaos);
            engine.start();
            while (engine.step()) {
                if (::testing::Test::HasFatalFailure()) return;
            }
            engine.finish();

            const Summary& s = engine.summary();
            const Portfolio& p = engine.portfolio();
            const double scale = std::max(1.0, s.turnover);
            ASSERT_NEAR(p.equity(), p.pnl_decomposed(), 1e-9 * scale) << "seed " << seed;
            ASSERT_LE(s.qty_filled, s.qty_submitted);
            Qty filled = 0;
            for (const auto& [id, m] : chaos.mine) filled += m.filled;
            // Fills still in flight to the strategy at the end are not in `mine`.
            ASSERT_LE(filled, s.qty_filled);
            for (InstrumentId i = 0; i < 2; ++i) {
                // Pre-trade risk bounds the worst case even through races.
                ASSERT_LE(std::abs(p.account(i).position), cfg.risk.max_position);
                for (const Side side : {Side::Buy, Side::Sell})
                    ASSERT_GE(engine.oms().open_qty(i, side), 0);
            }
            ASSERT_LE(engine.exchange().open_orders(), cfg.risk.max_open_orders);
            if (feed != &hostile) ASSERT_EQ(s.data_anomalies, 0u);
        }
    }
}

TEST(Fuzz, HostileFeedRunsAreStillDeterministic) {
    const FeedData feed = hostile_feed(77, 6000);
    std::uint64_t hashes[2];
    for (std::uint64_t& h : hashes) {
        EngineConfig cfg;
        cfg.latency.md_delay_ns = 40;
        cfg.latency.order_delay_ns = 90;
        cfg.latency.order_jitter_ns = 60;
        cfg.latency.response_delay_ns = 70;
        Chaos chaos(5, cfg.latency);
        Engine engine(feed, cfg, chaos);
        engine.run();
        h = engine.summary().event_hash;
        ASSERT_GT(engine.summary().fills, 50u);
    }
    EXPECT_EQ(hashes[0], hashes[1]);
}

// With an exact L3 feed and a single resting order, the simulator's answer
// can be checked against a brute-force model of the queue.
TEST(Property, L3QueuePositionMatchesBruteForceFifoModel) {
    Rng rng(4242);
    int none = 0, partial = 0, full = 0;
    for (int round = 0; round < 300; ++round) {
        const FeedData feed = generate_l3({.seed = 1000 + static_cast<std::uint64_t>(round),
                                           .events = 600,
                                           .initial_mid = 100,
                                           .mean_gap_ns = 10});
        const std::size_t join_at = 100 + rng.below(200);
        const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
        const Qty qty = rng.chance(50) ? rng.range(1, 60) : rng.range(200, 3000);

        Recorder rec;
        ExchangeConfig xc;
        xc.fill_on_crossing_quote = false; // isolate the queue model
        Exchange ex(1, xc, rec);

        // Brute force: remember exactly which orders were at our price when we
        // joined. We fill from any execution at our price that is not one of
        // them, or any execution at a strictly worse price.
        std::unordered_map<OrderId, std::pair<Side, Price>> info;
        std::unordered_map<OrderId, bool> ahead;
        Price px = 0;
        Qty expect = 0;
        bool joined = false;
        for (std::size_t k = 0; k < feed.events.size(); ++k) {
            const MarketEvent& e = feed.events[k];
            if (k == join_at) {
                const Level* own = ex.book(0).side(side).best();
                ASSERT_NE(own, nullptr);
                px = own->price; // join the touch: never marketable
                for (const auto& [id, sp] : info)
                    if (sp.first == side && sp.second == px) ahead[id] = true;
                ex.on_new_order(limit(1, side, px, qty), e.ts);
                joined = true;
            }
            if (e.type == MdType::Add) info[e.order_id] = {e.side, e.price};
            if (joined && e.type == MdType::Execute && info[e.order_id].first == side) {
                const Price epx = info[e.order_id].second;
                const bool through = better(side, px, epx);
                if (through || (epx == px && !ahead.contains(e.order_id)))
                    expect = std::min(qty, expect + e.qty);
            }
            ex.on_market_event(e);
            if (e.type == MdType::Delete) info.erase(e.order_id);
        }
        ASSERT_EQ(rec.filled(1), expect) << "round " << round;
        ++(expect == 0 ? none : expect < qty ? partial : full);
    }
    // The comparison only means something if all three outcomes occur.
    EXPECT_GT(none, 10);
    EXPECT_GT(partial, 10);
    EXPECT_GT(full, 10);
    RecordProperty("none", none);
    RecordProperty("partial", partial);
    RecordProperty("full", full);
}
