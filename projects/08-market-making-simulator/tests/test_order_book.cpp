#include "mmsim/order_book.hpp"
#include "test_framework.hpp"

#include <array>
#include <random>

using namespace mmsim;

TEST(FlatArrayBookBasicOperationsAndRecenter) {
    FlatArrayBook book(128);
    EXPECT_FALSE(book.top_of_book().valid());

    EXPECT_EQ(book.apply_delta(Side::Bid, 10000, 300), 0);
    EXPECT_EQ(book.apply_delta(Side::Bid, 9999, 400), 0);
    EXPECT_EQ(book.apply_delta(Side::Ask, 10002, 250), 0);
    EXPECT_EQ(book.apply_delta(Side::Ask, 10003, 500), 0);

    TopOfBook tob = book.top_of_book();
    EXPECT_TRUE(tob.valid());
    EXPECT_EQ(tob.best_bid, 10000);
    EXPECT_EQ(tob.bid_qty, 300);
    EXPECT_EQ(tob.best_ask, 10002);
    EXPECT_EQ(tob.ask_qty, 250);

    // Delete best bid -> falls back to 9999.
    EXPECT_EQ(book.apply_delta(Side::Bid, 10000, 0), 300);
    tob = book.top_of_book();
    EXPECT_EQ(tob.best_bid, 9999);
    EXPECT_EQ(tob.bid_qty, 400);

    // Jump far outside initial window of 128 ticks to test lossless recentering.
    EXPECT_EQ(book.apply_delta(Side::Ask, 10500, 100), 0);
    EXPECT_EQ(book.level_qty(Side::Bid, 9999), 400);
    EXPECT_EQ(book.level_qty(Side::Ask, 10002), 250);
    EXPECT_EQ(book.level_qty(Side::Ask, 10500), 100);
    EXPECT_EQ(book.top_of_book().best_ask, 10002);
}

TEST(DifferentialFlatArrayVsMapBook) {
    FlatArrayBook flat(256);
    MapBook ref;

    std::mt19937_64 rng(424242);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<PriceTicks> offset_dist(-40, 40);
    std::uniform_int_distribution<Qty> qty_dist(0, 800);

    PriceTicks center = 10000;
    std::array<PriceLevel, kMaxFeatureDepth> fl{};
    std::array<PriceLevel, kMaxFeatureDepth> ml{};

    for (int step = 0; step < 50'000; ++step) {
        if (step % 5000 == 0 && step > 0) {
            // Periodic price drift to exercise recentering.
            center += 150;
        }
        const Side side = side_dist(rng) == 0 ? Side::Bid : Side::Ask;
        const PriceTicks px = center + offset_dist(rng);
        const Qty qty = (step % 4 == 0) ? 0 : qty_dist(rng);

        const Qty old_flat = flat.apply_delta(side, px, qty);
        const Qty old_ref = ref.apply_delta(side, px, qty);
        EXPECT_EQ(old_flat, old_ref);

        const TopOfBook tf = flat.top_of_book();
        const TopOfBook tr = ref.top_of_book();
        EXPECT_EQ(tf.best_bid, tr.best_bid);
        EXPECT_EQ(tf.bid_qty, tr.bid_qty);
        EXPECT_EQ(tf.best_ask, tr.best_ask);
        EXPECT_EQ(tf.ask_qty, tr.ask_qty);
        EXPECT_EQ(flat.level_qty(side, px), ref.level_qty(side, px));

        if (step % 10 == 0) {
            const size_t nf_b = flat.top_levels(Side::Bid, 5, fl);
            const size_t nm_b = ref.top_levels(Side::Bid, 5, ml);
            EXPECT_EQ(nf_b, nm_b);
            for (size_t i = 0; i < nf_b; ++i) {
                EXPECT_EQ(fl[i].price_ticks, ml[i].price_ticks);
                EXPECT_EQ(fl[i].qty, ml[i].qty);
            }
            const size_t nf_a = flat.top_levels(Side::Ask, 5, fl);
            const size_t nm_a = ref.top_levels(Side::Ask, 5, ml);
            EXPECT_EQ(nf_a, nm_a);
            for (size_t i = 0; i < nf_a; ++i) {
                EXPECT_EQ(fl[i].price_ticks, ml[i].price_ticks);
                EXPECT_EQ(fl[i].qty, ml[i].qty);
            }
        }
    }
}

MMSIM_TEST_MAIN()
