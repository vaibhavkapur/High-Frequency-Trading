#include <gtest/gtest.h>

#include "sor/config.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/quote_normalizer.hpp"
#include "sor/venue_health.hpp"

using namespace sor;

TEST(QuoteNormalizerTest, WireEncodeDecodeRoundTrip) {
    NormalizedQuoteUpdate orig{};
    orig.venue_id = 1;
    orig.symbol_id = 42;
    orig.seq = 12345;
    orig.exchange_ts_ns = 1'000'000'000;
    orig.recv_ts_ns = 1'000'120'000;
    orig.type = QuoteUpdateType::Snapshot;
    orig.side = Side::Sell;
    orig.level_count = 2;
    orig.levels[0] = PriceLevel{usd_to_ticks(150.01), 300, 4};
    orig.levels[1] = PriceLevel{usd_to_ticks(150.02), 500, 7};

    const auto bytes = encode_wire_quote(orig);
    const auto decoded = decode_wire_quote(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.update.venue_id, orig.venue_id);
    EXPECT_EQ(decoded.update.symbol_id, orig.symbol_id);
    EXPECT_EQ(decoded.update.seq, orig.seq);
    EXPECT_EQ(decoded.update.exchange_ts_ns, orig.exchange_ts_ns);
    EXPECT_EQ(decoded.update.recv_ts_ns, orig.recv_ts_ns);
    EXPECT_EQ(decoded.update.type, orig.type);
    EXPECT_EQ(decoded.update.side, orig.side);
    EXPECT_EQ(decoded.update.level_count, 2);
    EXPECT_EQ(decoded.update.levels[0].price, orig.levels[0].price);
    EXPECT_EQ(decoded.update.levels[0].quantity, orig.levels[0].quantity);
    EXPECT_EQ(decoded.update.levels[1].price, orig.levels[1].price);
    EXPECT_EQ(decoded.update.levels[1].order_count, orig.levels[1].order_count);
}

TEST(QuoteNormalizerTest, RejectsCorruptedWireFrames) {
    NormalizedQuoteUpdate orig{};
    orig.venue_id = 0;
    orig.seq = 1;
    orig.exchange_ts_ns = 100;
    orig.recv_ts_ns = 200;
    orig.type = QuoteUpdateType::UpsertLevel;
    orig.side = Side::Buy;
    orig.level_count = 1;
    orig.levels[0] = PriceLevel{1'500'000, 100, 1};

    auto valid = encode_wire_quote(orig);

    // Truncated packet.
    EXPECT_EQ(decode_wire_quote(std::span<const std::uint8_t>(valid.data(), 10)).error,
              DecodeError::Truncated);

    // Bad magic.
    auto bad_magic = valid;
    bad_magic[0] ^= 0xFFU;
    EXPECT_EQ(decode_wire_quote(bad_magic).error, DecodeError::BadMagic);

    // Bad version.
    auto bad_ver = valid;
    bad_ver[4] = 99;
    EXPECT_EQ(decode_wire_quote(bad_ver).error, DecodeError::UnsupportedVersion);

    // Bad checksum (flip a payload byte).
    auto bad_csum = valid;
    bad_csum[kWireHeaderBytes] ^= 0x01U;
    EXPECT_EQ(decode_wire_quote(bad_csum).error, DecodeError::ChecksumMismatch);

    // Trailing garbage byte -> LengthMismatch.
    auto extra = valid;
    extra.push_back(0x00);
    EXPECT_EQ(decode_wire_quote(extra).error, DecodeError::LengthMismatch);
}

TEST(QuoteNormalizerTest, SequenceGapOutOfOrderAndClockSkew) {
    QuoteNormalizer norm;
    norm.register_venue(0, 100.0);  // Venue sends cents; canonical is 1e-4 USD (x100).

    NormalizedQuoteUpdate raw{};
    raw.venue_id = 0;
    raw.seq = 10;
    raw.exchange_ts_ns = 5000;
    raw.recv_ts_ns = 4000;  // Negative apparent feed latency -> clamped to 5000.
    raw.type = QuoteUpdateType::UpsertLevel;
    raw.side = Side::Buy;
    raw.level_count = 1;
    raw.levels[0] = PriceLevel{15000, 200, 1};  // $150.00 in cents

    NormalizedQuoteUpdate out{};
    ASSERT_TRUE(norm.normalize(raw, out));
    EXPECT_EQ(out.recv_ts_ns, 5000);
    EXPECT_EQ(out.levels[0].price, 1'500'000);
    EXPECT_EQ(norm.stats().negative_latency_clamped, 1U);

    // Out-of-order / duplicate sequence dropped.
    raw.seq = 10;
    EXPECT_FALSE(norm.normalize(raw, out));
    EXPECT_EQ(norm.stats().out_of_order_dropped, 1U);

    // Sequence gap detected (10 -> 15).
    raw.seq = 15;
    raw.recv_ts_ns = 6000;
    ASSERT_TRUE(norm.normalize(raw, out));
    EXPECT_EQ(norm.stats().sequence_gaps, 1U);
    EXPECT_EQ(norm.stats().messages_accepted, 2U);
}

TEST(ConsolidatedBookTest, MaintainsSortedLevelsAndFeeAdjustedBBO) {
    auto cfg = make_default_config();
    ConsolidatedBook book;
    book.configure_venues(cfg.venues);

    const TimestampNs now = 10'000'000'000;
    // Venue 0 (ARCA, +$0.0030 fee): Bid $149.99, Ask $150.0000
    book.set_level(0, Side::Buy, usd_to_ticks(149.98), 300, now - 200'000, now - 100'000);
    book.set_level(0, Side::Buy, usd_to_ticks(149.99), 200, now - 200'000, now - 100'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.01), 400, now - 200'000, now - 100'000);
    book.set_level(0, Side::Sell, usd_to_ticks(150.00), 250, now - 200'000, now - 100'000);

    // Venue 1 (BATS_INV, -$0.0018 rebate): Bid $149.985, Ask $150.0020
    // Effective ask on ARCA = 150.0030; effective ask on BATS_INV = 150.0002 -> BATS_INV wins!
    book.set_level(1, Side::Buy, usd_to_ticks(149.9850), 300, now - 200'000, now - 100'000);
    book.set_level(1, Side::Sell, usd_to_ticks(150.0020), 300, now - 200'000, now - 100'000);

    // Venue 2 (IEX_SIM): stale quote (10ms old > 5ms stale timeout) with $149.9950 ask.
    book.set_level(2, Side::Sell, usd_to_ticks(149.9950), 500, now - 10'500'000, now - 10'000'000);

    EXPECT_FALSE(book.is_venue_stale(0, now));
    EXPECT_FALSE(book.is_venue_stale(1, now));
    EXPECT_TRUE(book.is_venue_stale(2, now));

    // Verify sorting within ARCA book.
    EXPECT_EQ(book.venue_book(0).best_bid().price, usd_to_ticks(149.99));
    EXPECT_EQ(book.venue_book(0).best_ask().price, usd_to_ticks(150.00));

    // Consolidated BBO excludes stale Venue 2.
    const auto bbo = book.consolidated_bbo(now, nullptr, true, true);
    EXPECT_EQ(bbo.eligible_venues, 2U);
    EXPECT_EQ(bbo.best_ask_venue, 0);
    EXPECT_EQ(bbo.best_ask_price, usd_to_ticks(150.00));
    EXPECT_EQ(bbo.best_eff_ask_venue, 1);  // Fee-adjusted best ask is BATS_INV!
    EXPECT_LT(bbo.best_eff_ask_ticks, cfg.venues[0].effective_price_ticks(Side::Buy, usd_to_ticks(150.00)));

    // Deleting top level exposes second level.
    book.set_level(0, Side::Sell, usd_to_ticks(150.00), 0, now - 50'000, now - 10'000);
    EXPECT_EQ(book.venue_book(0).best_ask().price, usd_to_ticks(150.01));
}
