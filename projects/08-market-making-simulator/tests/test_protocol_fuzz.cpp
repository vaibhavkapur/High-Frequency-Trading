#include "mmsim/feed.hpp"
#include "mmsim/protocol.hpp"
#include "test_framework.hpp"

#include <array>
#include <cstdio>
#include <random>
#include <vector>

using namespace mmsim;

TEST(HeaderAndRecordRoundTrip) {
    BinFileHeader hdr{};
    hdr.version = kBinVersion;
    hdr.flags = 0;
    hdr.tick_size_micros = 10'000;

    std::array<uint8_t, kBinHeaderSize> hbuf{};
    encode_bin_header(hdr, hbuf);

    BinFileHeader decoded_hdr{};
    EXPECT_EQ(static_cast<int>(decode_bin_header(hbuf, decoded_hdr)),
              static_cast<int>(DecodeStatus::Ok));
    EXPECT_EQ(decoded_hdr.version, hdr.version);
    EXPECT_EQ(decoded_hdr.tick_size_micros, hdr.tick_size_micros);

    const std::vector<MarketEvent> samples = {
        {0, 1, MarketEventType::BookClear, Side::Bid, 0, 0},
        {123456789, 2, MarketEventType::BookDelta, Side::Bid, 10000, 350},
        {123456999, 3, MarketEventType::BookDelta, Side::Ask, 10002, 0},
        {200000000, 4, MarketEventType::Trade, Side::Ask, 10000, 120},
    };
    for (const auto& ev : samples) {
        std::array<uint8_t, kBinRecordSize> rbuf{};
        encode_bin_record(ev, rbuf);
        MarketEvent out{};
        EXPECT_EQ(static_cast<int>(decode_bin_record(rbuf, out)),
                  static_cast<int>(DecodeStatus::Ok));
        EXPECT_EQ(out.timestamp_ns, ev.timestamp_ns);
        EXPECT_EQ(out.seq_no, ev.seq_no);
        EXPECT_EQ(static_cast<int>(out.type), static_cast<int>(ev.type));
        EXPECT_EQ(static_cast<int>(out.side), static_cast<int>(ev.side));
        EXPECT_EQ(out.price_ticks, ev.price_ticks);
        EXPECT_EQ(out.qty, ev.qty);
    }
}

TEST(SingleBitFlipCaughtByCrc) {
    MarketEvent ev{987654321, 77, MarketEventType::Trade, Side::Bid, 10005, 250};
    std::array<uint8_t, kBinRecordSize> rbuf{};
    encode_bin_record(ev, rbuf);

    for (size_t byte_idx = 0; byte_idx < kBinRecordSize; ++byte_idx) {
        for (uint8_t bit = 0; bit < 8; ++bit) {
            auto corrupted = rbuf;
            corrupted[byte_idx] ^= static_cast<uint8_t>(1u << bit);
            MarketEvent out{};
            const DecodeStatus st = decode_bin_record(corrupted, out);
            EXPECT_EQ(static_cast<int>(st), static_cast<int>(DecodeStatus::BadChecksum));
        }
    }
}

TEST(FuzzArbitraryBinaryInputsNeverCrashAndValidateStrictly) {
    std::mt19937_64 rng(0xC0FFEE12345ULL);
    std::uniform_int_distribution<size_t> len_dist(0, 64);
    std::uniform_int_distribution<unsigned int> byte_dist(0, 255);

    for (int iter = 0; iter < 50'000; ++iter) {
        const size_t n = len_dist(rng);
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; ++i) {
            buf[i] = static_cast<uint8_t>(byte_dist(rng));
        }
        BinFileHeader hdr{};
        const auto hs = decode_bin_header(buf, hdr);
        if (hs == DecodeStatus::Ok) {
            EXPECT_EQ(hdr.version, kBinVersion);
            EXPECT_TRUE(hdr.tick_size_micros > 0);
        }

        MarketEvent ev{};
        const auto rs = decode_bin_record(buf, ev);
        if (rs == DecodeStatus::Ok) {
            EXPECT_GE(ev.timestamp_ns, 0);
            EXPECT_GE(ev.qty, 0);
        }
    }
}

TEST(RejectInvalidFieldsWithValidCrc) {
    // Construct a record with invalid fields (negative qty) and recompute valid CRC.
    MarketEvent bad_ev{1000, 1, MarketEventType::Trade, Side::Bid, 10000, 0};
    std::array<uint8_t, kBinRecordSize> rbuf{};
    encode_bin_record(bad_ev, rbuf); // Trade with qty == 0 must be rejected on decode.
    MarketEvent out{};
    EXPECT_EQ(static_cast<int>(decode_bin_record(rbuf, out)),
              static_cast<int>(DecodeStatus::InvalidField));

    // Truncated header / record checks.
    BinFileHeader hdr{};
    EXPECT_EQ(static_cast<int>(decode_bin_header(std::span<const uint8_t>(rbuf.data(), 8), hdr)),
              static_cast<int>(DecodeStatus::Truncated));
    EXPECT_EQ(static_cast<int>(decode_bin_record(std::span<const uint8_t>(rbuf.data(), 20), out)),
              static_cast<int>(DecodeStatus::Truncated));
}

TEST(BinaryFeedSaveLoadAndOutOfOrderReject) {
    const std::string path = "/tmp/mmsim_test_feed.mmbin";
    const std::vector<MarketEvent> good = {
        {100, 1, MarketEventType::BookDelta, Side::Bid, 9999, 200},
        {200, 2, MarketEventType::BookDelta, Side::Ask, 10001, 200},
        {300, 3, MarketEventType::Trade, Side::Bid, 10001, 50},
    };
    std::string err;
    EXPECT_TRUE(save_market_events_bin(path, good, 0.01, err));

    std::vector<MarketEvent> loaded;
    double tick = 0.0;
    EXPECT_TRUE(load_market_events(path, loaded, &tick, err));
    EXPECT_EQ(loaded.size(), good.size());
    EXPECT_NEAR(tick, 0.01, 1e-9);

    // Out-of-order timestamps must be rejected.
    const std::vector<MarketEvent> unordered = {
        {500, 1, MarketEventType::BookDelta, Side::Bid, 9999, 200},
        {400, 2, MarketEventType::BookDelta, Side::Ask, 10001, 200},
    };
    EXPECT_TRUE(save_market_events_bin(path, unordered, 0.01, err));
    EXPECT_FALSE(load_market_events(path, loaded, &tick, err));
    std::remove(path.c_str());
}

MMSIM_TEST_MAIN()
