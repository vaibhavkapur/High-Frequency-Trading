#include <gtest/gtest.h>

#include "hftbt/synth.hpp"
#include "test_util.hpp"

using namespace hftbt;
using namespace hftbt::test;

namespace {
std::span<const std::byte> bytes(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }
} // namespace

TEST(FeedCsv, ParsesAllEventTypes) {
    const char* csv = "ts_ns,symbol,type,order_id,side,price,qty\n"
                      "# comment\n"
                      "100,AAA,A,1,B,1000,5\n"
                      "100,BBB,A,2,S,2000,7\r\n"
                      "150,AAA,C,1,,,2\n"
                      "160,AAA,E,1,,,1\n"
                      "170,AAA,D,1,,,\n"
                      "180,BBB,L,,S,2001,9\n"
                      "190,BBB,T,,B,2001,3\n";
    FeedData f;
    std::string err;
    ASSERT_TRUE(parse_csv(csv, f, err)) << err;
    ASSERT_EQ(f.symbols, (std::vector<std::string>{"AAA", "BBB"}));
    ASSERT_EQ(f.events.size(), 7u);
    EXPECT_EQ(f.events[0], add(100, 1, Side::Buy, 1000, 5, 0));
    EXPECT_EQ(f.events[1], add(100, 2, Side::Sell, 2000, 7, 1));
    EXPECT_EQ(f.events[2].type, MdType::Cancel);
    EXPECT_EQ(f.events[2].qty, 2);
    EXPECT_EQ(f.events[5], level(180, Side::Sell, 2001, 9, 1));
    EXPECT_EQ(f.events[6], trade(190, Side::Buy, 2001, 3, 1));
}

TEST(FeedCsv, RejectsMalformedInput) {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"100,AAA,A,1,B,1000\n", "7 comma-separated"},
        {"100,AAA,A,1,B,1000,5,extra\n", "7 comma-separated"},
        {"abc,AAA,A,1,B,1000,5\n", "bad ts_ns"},
        {"100,AAA,Z,1,B,1000,5\n", "bad type"},
        {"100,AAA,A,1,X,1000,5\n", "bad side"},
        {"100,AAA,A,1,,1000,5\n", "side is required"},
        {"100,AAA,A,1,B,10.5,5\n", "bad price"},
        {"100,AAA,A,1,B,1000,-5\n", "qty out of range"},
        {"100,AAA,A,1,B,-1,5\n", "price out of range"},
        {"200,AAA,A,1,B,1000,5\n100,AAA,A,2,B,1000,5\n", "goes backwards"},
        {"-5,AAA,A,1,B,1000,5\n", "negative timestamp"},
    };
    for (const auto& [text, expect] : cases) {
        FeedData f;
        std::string err;
        EXPECT_FALSE(parse_csv(text, f, err)) << text;
        EXPECT_NE(err.find(expect), std::string::npos) << "got: " << err;
    }
}

TEST(FeedCsv, RoundTrip) {
    const FeedData f = generate_l3({.seed = 3, .events = 2000, .instruments = 3});
    FeedData g;
    std::string err;
    ASSERT_TRUE(parse_csv(to_csv(f), g, err)) << err;
    EXPECT_EQ(f.symbols, g.symbols);
    EXPECT_EQ(f.events, g.events);
}

TEST(FeedBin, RoundTrip) {
    const FeedData f = l3_to_l2(generate_l3({.seed = 4, .events = 2000, .instruments = 2}));
    FeedData g;
    std::string err;
    ASSERT_TRUE(parse_bin(bytes(to_bin(f)), g, err)) << err;
    EXPECT_EQ(f.symbols, g.symbols);
    EXPECT_EQ(f.events, g.events);
}

TEST(FeedBin, RejectsCorruptInput) {
    const FeedData f = generate_l3({.seed = 5, .events = 50});
    const std::vector<std::byte> good = to_bin(f);
    FeedData g;
    std::string err;

    EXPECT_FALSE(parse_bin({}, g, err));

    auto bad = good;
    bad[0] = std::byte{'X'};
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));
    EXPECT_NE(err.find("magic"), std::string::npos);

    bad = good;
    bad[8] = std::byte{9}; // version
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));
    EXPECT_NE(err.find("version"), std::string::npos);

    bad = good;
    bad.pop_back(); // truncated record
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));
    EXPECT_NE(err.find("payload size"), std::string::npos);

    bad = good;
    bad[16] = std::byte{0xFF}; // event count far larger than the payload
    bad[23] = std::byte{0xFF};
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));

    bad = good;
    bad[bad.size() - 2] = std::byte{'?'}; // type byte of the last record
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));
    EXPECT_NE(err.find("type/side"), std::string::npos);
    EXPECT_TRUE(g.events.empty()); // nothing half-parsed leaks out

    bad = good;
    bad[bad.size() - 6] = std::byte{0x7F}; // instrument id out of range
    EXPECT_FALSE(parse_bin(bytes(bad), g, err));
    EXPECT_NE(err.find("instrument"), std::string::npos);
}

TEST(Synth, SameSeedSameFeedDifferentSeedDifferentFeed) {
    const SynthConfig c{.seed = 11, .events = 5000, .instruments = 2};
    SynthConfig d = c;
    d.seed = 12;
    EXPECT_EQ(generate_l3(c).events, generate_l3(c).events);
    EXPECT_NE(generate_l3(c).events, generate_l3(d).events);
}

TEST(Synth, GeneratedBookIsNeverCrossedAndFullyConsistent) {
    const FeedData f = generate_l3({.seed = 21, .events = 50000, .instruments = 3});
    ASSERT_EQ(f.events.size(), 50000u);
    std::string err;
    ASSERT_TRUE(validate_feed(f, err)) << err;
    std::vector<OrderBook> books(3);
    for (const MarketEvent& e : f.events) {
        OrderBook& b = books[e.instrument];
        ASSERT_TRUE(b.apply(e).ok);
        if (b.best_bid() && b.best_ask()) ASSERT_LT(b.best_bid()->price, b.best_ask()->price);
    }
}

TEST(Synth, L2ConversionReproducesTheSameLevels) {
    const FeedData l3 = generate_l3({.seed = 22, .events = 20000});
    const FeedData l2 = l3_to_l2(l3);
    OrderBook a, b;
    for (const MarketEvent& e : l3.events) a.apply(e);
    for (const MarketEvent& e : l2.events) b.apply(e);
    for (const Side s : {Side::Buy, Side::Sell}) {
        ASSERT_EQ(a.side(s).size(), b.side(s).size());
        for (std::size_t i = 0; i < a.side(s).size(); ++i) {
            EXPECT_EQ(a.side(s).from_best(i).price, b.side(s).from_best(i).price);
            EXPECT_EQ(a.side(s).from_best(i).qty, b.side(s).from_best(i).qty);
        }
    }
}
