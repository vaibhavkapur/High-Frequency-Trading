#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#include "test_harness.hpp"
#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/json_lite.hpp"
#include "xarb/replay_engine.hpp"
#include "xarb/synth_feed.hpp"
#include "xarb/venue_adapter.hpp"

using namespace xarb;

XARB_TEST(RecordedFeedReplayIsDeterministicAcrossJsonlAndBinary) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  SynthFeedConfig scfg;
  scfg.num_steps = 1200;
  scfg.seed = 987654321ULL;
  scfg.inject_faults = true;
  auto feed = generate_synthetic_feed(reg, scfg);
  EXPECT_GT(feed.size(), 3000U);

  const std::string tmp_jsonl = "/tmp/xarb_test_det.jsonl";
  const std::string tmp_bin = "/tmp/xarb_test_det.xarb";
  std::string err;
  EXPECT_TRUE(write_jsonl_feed(tmp_jsonl, feed, &err));
  EXPECT_TRUE(write_binary_feed(tmp_bin, feed, &err));

  std::vector<RecordedFeedEvent> feed_jsonl, feed_bin;
  EXPECT_TRUE(read_feed_auto(tmp_jsonl, feed_jsonl, &err));
  EXPECT_TRUE(read_feed_auto(tmp_bin, feed_bin, &err));
  EXPECT_EQ(feed_jsonl.size(), feed.size());
  EXPECT_EQ(feed_bin.size(), feed.size());

  ReplayConfig rcfg;
  rcfg.rng_seed = 777;
  rcfg.paper.order_latency_ns = {4LL * kNsPerMs, 6LL * kNsPerMs, 5LL * kNsPerMs};

  ReplayEngine e1(reg, rcfg);
  ReplayEngine e2(reg, rcfg);
  ReplayEngine e3(reg, rcfg);
  auto r1 = e1.run(feed_jsonl, tmp_jsonl);
  auto r2 = e2.run(feed_jsonl, tmp_jsonl);
  auto r3 = e3.run(feed_bin, tmp_bin);

  EXPECT_EQ(r1.summary.run_hash, r2.summary.run_hash);
  EXPECT_EQ(r1.summary.run_hash, r3.summary.run_hash);
  EXPECT_EQ(r1.signals.size(), r2.signals.size());
  EXPECT_EQ(r1.trades.size(), r3.trades.size());
  EXPECT_TRUE(r1.summary.accounting_invariants_ok);

  // Verify all 3 injected venue fault recovery paths actually fired!
  EXPECT_GE(r1.summary.venues[static_cast<size_t>(VenueId::Binance)].gaps_detected, 1ULL);
  EXPECT_GE(r1.summary.venues[static_cast<size_t>(VenueId::Coinbase)].stale_transitions, 1ULL);
  EXPECT_GE(r1.summary.venues[static_cast<size_t>(VenueId::Okx)].checksum_mismatches, 1ULL);

  std::error_code ec;
  std::filesystem::remove(tmp_jsonl, ec);
  std::filesystem::remove(tmp_bin, ec);
}

XARB_TEST(HigherLatencyIncreasesFalsePositiveRate) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  SynthFeedConfig scfg;
  scfg.num_steps = 2000;
  scfg.seed = 424242ULL;
  scfg.inject_faults = false;
  auto feed = generate_synthetic_feed(reg, scfg);

  ReplayConfig low_lat;
  low_lat.paper.order_latency_ns = {0, 0, 0};
  low_lat.paper.order_jitter_ns = 0;

  ReplayConfig high_lat;
  high_lat.paper.order_latency_ns = {60LL * kNsPerMs, 60LL * kNsPerMs, 60LL * kNsPerMs};
  high_lat.paper.order_jitter_ns = 0;

  ReplayEngine e_low(reg, low_lat);
  ReplayEngine e_high(reg, high_lat);
  auto r_low = e_low.run(feed);
  auto r_high = e_high.run(feed);

  EXPECT_GT(r_low.summary.paper.trades_completed, 10ULL);
  EXPECT_GT(r_high.summary.paper.trades_completed, 10ULL);
  EXPECT_GT(r_high.summary.false_positive_rate, r_low.summary.false_positive_rate);
  EXPECT_LT(r_high.summary.paper.net_simulated_pnl_quote, r_low.summary.paper.net_simulated_pnl_quote);
}

XARB_TEST(FuzzBinaryCodecJsonParserAndVenueAdapters) {
  std::mt19937_64 rng(0xDEADBEEFULL);
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  const auto& btc = *reg.get(0);

  // 1. Binary frame fuzzing: flip random bits, truncate, corrupt header/CRC
  RecordedFeedEvent sample{1'700'000'000'000'000LL, VenueId::Binance, 0, FeedEventKind::WsMessage,
                           R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":1,"b":[],"a":[]})"};
  auto valid_frame = encode_binary_frame(sample);
  {
    size_t off = 0;
    RecordedFeedEvent decoded;
    EXPECT_TRUE(decode_binary_frame(valid_frame.data(), valid_frame.size(), off, decoded));
    EXPECT_EQ(off, valid_frame.size());
    EXPECT_EQ(decoded.payload, sample.payload);
  }

  for (int i = 0; i < 4000; ++i) {
    auto buf = valid_frame;
    const uint64_t mode = rng() % 4ULL;
    if (mode == 0) {
      // Single bit flip anywhere -> MUST fail CRC32 or header check!
      size_t byte_idx = rng() % buf.size();
      buf[byte_idx] ^= static_cast<uint8_t>(1U << (rng() % 8U));
      size_t off = 0;
      RecordedFeedEvent dec;
      EXPECT_FALSE(decode_binary_frame(buf.data(), buf.size(), off, dec));
    } else if (mode == 1) {
      // Truncate
      size_t new_len = rng() % buf.size();
      buf.resize(new_len);
      size_t off = 0;
      RecordedFeedEvent dec;
      EXPECT_FALSE(decode_binary_frame(buf.data(), buf.size(), off, dec));
    } else {
      // Completely random bytes
      size_t rlen = rng() % 256;
      std::vector<uint8_t> rbuf(rlen);
      for (auto& b : rbuf) b = static_cast<uint8_t>(rng() & 0xFFU);
      size_t off = 0;
      RecordedFeedEvent dec;
      (void)decode_binary_frame(rbuf.data(), rbuf.size(), off, dec);
    }
  }

  // 2. JSON parser fuzzing: deep nesting + random ASCII/control bytes
  {
    std::string deep(200, '[');
    deep.append(200, ']');
    auto res = parse_json(deep, 32);
    EXPECT_FALSE(res.ok());
  }

  BinanceAdapter bin(btc, 1000LL * kNsPerMs);
  CoinbaseAdapter cb(btc, 1000LL * kNsPerMs);
  OkxAdapter okx(btc, 1000LL * kNsPerMs);

  static constexpr char kAlphabet[] = R"({}[]":,.-0123456789abcdeftnul\ )";
  for (int i = 0; i < 3000; ++i) {
    size_t len = rng() % 180;
    std::string s(len, ' ');
    for (size_t k = 0; k < len; ++k) {
      s[k] = kAlphabet[rng() % (sizeof(kAlphabet) - 1)];
    }
    (void)parse_json(s);
    (void)bin.on_ws_message(s, 1000LL * kNsPerMs + i);
    (void)bin.on_rest_snapshot(s, 1000LL * kNsPerMs + i);
    (void)cb.on_ws_message(s, 1000LL * kNsPerMs + i);
    (void)okx.on_ws_message(s, 1000LL * kNsPerMs + i);
  }
}

int main() { return xarb::test::run_all("test_replay_and_fuzz"); }
