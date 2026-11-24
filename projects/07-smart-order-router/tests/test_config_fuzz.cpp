#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/json_util.hpp"
#include "sor/quote_normalizer.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

using namespace sor;

TEST(ConfigTest, ParsesValidJsonAndRejectsMalformed) {
    const std::string valid = R"({
      "venues": [
        {
          "id": 0,
          "name": "ALPHA",
          "taker_fee_per_share_ticks": 25,
          "base_one_way_latency_ns": 90000,
          "stale_timeout_ns": 3000000,
          "lot_size": 10,
          "min_order_qty": 10,
          "max_child_qty": 1000
        }
      ],
      "router": {
        "policy": "fee_aware",
        "max_reroute_attempts": 5
      },
      "risk": {
        "max_order_qty": 25000
      },
      "simulation": {
        "scenario": "fee_inversion",
        "seed": 777,
        "num_parent_orders": 50
      }
    })";

    SystemConfig cfg{};
    std::string err;
    ASSERT_TRUE(load_config_from_json(valid, cfg, err)) << err;
    ASSERT_EQ(cfg.venues.size(), 1U);
    EXPECT_EQ(cfg.venues[0].name, "ALPHA");
    EXPECT_EQ(cfg.venues[0].lot_size, 10);
    EXPECT_EQ(cfg.router.policy, RoutingPolicy::FeeAware);
    EXPECT_EQ(cfg.router.max_reroute_attempts, 5U);
    EXPECT_EQ(cfg.risk.max_order_qty, 25000);
    EXPECT_EQ(cfg.sim.scenario, ScenarioType::FeeInversion);
    EXPECT_EQ(cfg.sim.seed, 777U);

    // Invalid policy rejected.
    EXPECT_FALSE(load_config_from_json(R"({"router":{"policy":"bogus"}})", cfg, err));
    // Malformed syntax rejected.
    EXPECT_FALSE(load_config_from_json("{invalid json", cfg, err));
    // Excessive nesting rejected cleanly without stack overflow.
    std::string deep(200, '[');
    deep += std::string(200, ']');
    JsonValue jv;
    EXPECT_FALSE(parse_json(deep, jv, err));
}

TEST(FuzzTest, BinaryWireQuoteDecoderNeverCrashesOnRandomOrMutatedInputs) {
    std::mt19937_64 rng(0xDEADBEEFCAFEULL);
    std::uniform_int_distribution<std::size_t> len_dist(0, 512);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    // 1) 5,000 completely random byte buffers.
    for (int i = 0; i < 5000; ++i) {
        const std::size_t len = len_dist(rng);
        std::vector<std::uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<std::uint8_t>(byte_dist(rng));
        const auto res = decode_wire_quote(buf);
        if (res.ok()) {
            EXPECT_LT(res.update.venue_id, kMaxVenues);
            EXPECT_LE(res.update.level_count, kMaxBookDepth);
        }
    }

    // 2) 5,000 targeted mutations of valid packets (bit flips, truncations, magic-preserving fuzz).
    NormalizedQuoteUpdate base{};
    base.venue_id = 2;
    base.symbol_id = 7;
    base.seq = 99;
    base.exchange_ts_ns = 1'000'000;
    base.recv_ts_ns = 1'050'000;
    base.type = QuoteUpdateType::Snapshot;
    base.side = Side::Buy;
    base.level_count = 3;
    base.levels[0] = {1'500'000, 100, 1};
    base.levels[1] = {1'499'900, 200, 2};
    base.levels[2] = {1'499'800, 300, 3};
    const auto valid_pkt = encode_wire_quote(base);

    // Every single bit flip in a valid packet must fail checksum or header validation.
    for (std::size_t byte_idx = 0; byte_idx < valid_pkt.size(); ++byte_idx) {
        auto mutated = valid_pkt;
        mutated[byte_idx] ^= 0x01U;
        EXPECT_FALSE(decode_wire_quote(mutated).ok()) << "bit flip at byte " << byte_idx;
    }

    QuoteNormalizer norm;
    for (VenueId v = 0; v < 4; ++v) norm.register_venue(v);

    for (int i = 0; i < 5000; ++i) {
        auto pkt = valid_pkt;
        const std::size_t flips = static_cast<std::size_t>(rng() % 5) + 1;
        for (std::size_t f = 0; f < flips; ++f) {
            pkt[rng() % pkt.size()] = static_cast<std::uint8_t>(byte_dist(rng));
        }
        if (rng() % 4 == 0) {
            pkt.resize(rng() % (pkt.size() + 1));
        }
        NormalizedQuoteUpdate out{};
        (void)norm.decode_and_normalize(pkt, out);
    }
}

TEST(FuzzTest, JsonParserNeverCrashesOnRandomInputs) {
    std::mt19937_64 rng(0x123456789ABCDEF0ULL);
    const std::string alphabet = R"({}[]":,0123456789.-+eEtruefalsn\u \n\t)";
    std::uniform_int_distribution<std::size_t> char_dist(0, alphabet.size() - 1);
    std::uniform_int_distribution<std::size_t> len_dist(0, 128);

    for (int i = 0; i < 2000; ++i) {
        const std::size_t len = len_dist(rng);
        std::string s(len, ' ');
        for (char& c : s) c = alphabet[char_dist(rng)];
        JsonValue val;
        std::string err;
        (void)parse_json(s, val, err);
    }
}

TEST(PropertyTest, RoutePlannerInvariantsHoldAcrossRandomizedBooks) {
    std::mt19937_64 rng(20261124ULL);
    auto cfg = make_default_config();
    cfg.venues[0].lot_size = 10;
    cfg.venues[0].min_order_qty = 20;
    cfg.venues[0].max_child_qty = 250;
    cfg.venues[1].lot_size = 5;
    cfg.venues[1].min_order_qty = 10;
    cfg.venues[1].max_child_qty = 300;
    cfg.venues[2].lot_size = 1;
    cfg.venues[2].min_order_qty = 1;
    cfg.venues[2].max_child_qty = 400;

    ConsolidatedBook book;
    book.configure_venues(cfg.venues);
    VenueHealthMonitor health;
    RoutePlanner planner;

    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Quantity> qty_dist(1, 2000);
    std::uniform_int_distribution<PriceTicks> px_off(-500, 500);
    std::uniform_int_distribution<Quantity> lvl_qty_dist(10, 600);
    std::uniform_int_distribution<int> state_dist(0, 5);

    const TimestampNs now = 100'000'000'000;

    for (int iter = 0; iter < 1000; ++iter) {
        book.clear_all();
        health.reset();

        for (VenueId vid = 0; vid < 3; ++vid) {
            const int mode = state_dist(rng);
            const bool make_stale = (mode == 0);
            const bool make_down = (mode == 1);
            if (make_down) health.set_manual_down(vid, true, now);

            const TimestampNs recv_ts = make_stale ? (now - 10'000'000) : (now - 100'000);
            for (int lvl = 0; lvl < 4; ++lvl) {
                book.set_level(vid, Side::Buy, 1'500'000 - 100 - lvl * 100 + px_off(rng),
                               lvl_qty_dist(rng), recv_ts - 50'000, recv_ts);
                book.set_level(vid, Side::Sell, 1'500'000 + 100 + lvl * 100 + px_off(rng),
                               lvl_qty_dist(rng), recv_ts - 50'000, recv_ts);
            }
        }

        RouteRequest req{};
        req.side = (side_dist(rng) == 0) ? Side::Buy : Side::Sell;
        req.qty = qty_dist(rng);
        req.limit_price = (req.side == Side::Buy) ? (1'500'000 + 350) : (1'500'000 - 350);
        req.tif = (iter % 7 == 0) ? TimeInForce::FOK : TimeInForce::IOC;
        req.now_ns = now;

        const auto plan = planner.plan(book, &health, req);

        // Invariant 1: exact quantity conservation.
        EXPECT_LE(plan.planned_qty, req.qty);
        EXPECT_EQ(plan.planned_qty + plan.unroutable_qty, req.qty);

        Quantity sum_slices = 0;
        for (std::size_t i = 0; i < plan.slice_count; ++i) {
            const auto& sl = plan.slices[i];
            const auto& vcfg = cfg.venues[sl.venue_id];
            sum_slices += sl.quantity;

            // Invariant 2: never routes to stale or down venue.
            EXPECT_FALSE(book.is_venue_stale(sl.venue_id, now));
            EXPECT_TRUE(health.is_routable(sl.venue_id, now));

            // Invariant 3: child order constraints.
            EXPECT_GE(sl.quantity, vcfg.min_order_qty);
            EXPECT_LE(sl.quantity, vcfg.max_child_qty);
            EXPECT_EQ(sl.quantity % vcfg.lot_size, 0);

            // Invariant 4: parent limit price respected.
            if (req.side == Side::Buy) {
                EXPECT_LE(sl.limit_price, req.limit_price);
            } else {
                EXPECT_GE(sl.limit_price, req.limit_price);
            }
        }
        EXPECT_EQ(sum_slices, plan.planned_qty);
    }
}
