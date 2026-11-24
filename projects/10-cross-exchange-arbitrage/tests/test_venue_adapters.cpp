#include "test_harness.hpp"
#include "xarb/crc32.hpp"
#include "xarb/instrument.hpp"
#include "xarb/venue_adapter.hpp"

using namespace xarb;

XARB_TEST(BinanceSnapshotDeltaBufferingAndGapRecovery) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  const auto& btc = *reg.get(0);
  BinanceAdapter adapter(btc, 1000LL * kNsPerMs);

  // 1. Incoming WS deltas before snapshot are buffered and trigger REST snapshot request.
  auto a1 = adapter.on_ws_message(
      R"({"e":"depthUpdate","E":1700000000100,"s":"BTCUSDT","U":98,"u":100,"b":[["65000.00","1.000000"]],"a":[]})",
      1000LL * kNsPerMs);
  EXPECT_TRUE(a1.needs_rest_snapshot);
  EXPECT_EQ(adapter.state(), BookState::Buffering);

  auto a2 = adapter.on_ws_message(
      R"({"e":"depthUpdate","E":1700000000110,"s":"BTCUSDT","U":101,"u":102,"b":[["65001.00","2.000000"]],"a":[]})",
      1010LL * kNsPerMs);
  EXPECT_FALSE(a2.book_updated);

  // 2. REST snapshot with lastUpdateId=100 drops [98..100] and applies [101..102]!
  auto snap_act = adapter.on_rest_snapshot(
      R"({"lastUpdateId":100,"bids":[["64999.00","1.500000"]],"asks":[["65005.00","1.000000"]]})",
      1020LL * kNsPerMs);
  EXPECT_TRUE(snap_act.book_updated);
  EXPECT_TRUE(snap_act.recovered_sync);
  EXPECT_EQ(adapter.state(), BookState::Synced);
  EXPECT_EQ(adapter.last_sequence(), 102ULL);
  EXPECT_EQ(adapter.book().best_bid()->price, 6500100LL);

  // 3. Duplicate/stale delta (u <= 102) ignored.
  auto dup = adapter.on_ws_message(
      R"({"e":"depthUpdate","E":1700000000115,"s":"BTCUSDT","U":101,"u":102,"b":[["64000.00","5.000000"]],"a":[]})",
      1025LL * kNsPerMs);
  EXPECT_FALSE(dup.book_updated);
  EXPECT_EQ(adapter.metrics().duplicates_ignored, 1ULL);

  // 4. Out-of-order / skipped sequence (U=105 != 103) triggers Resyncing and clears book!
  auto gap = adapter.on_ws_message(
      R"({"e":"depthUpdate","E":1700000000130,"s":"BTCUSDT","U":105,"u":106,"b":[["65002.00","1.000000"]],"a":[]})",
      1030LL * kNsPerMs);
  EXPECT_TRUE(gap.gap_detected);
  EXPECT_TRUE(gap.needs_rest_snapshot);
  EXPECT_EQ(adapter.state(), BookState::Resyncing);
  EXPECT_FALSE(adapter.is_tradable(1030LL * kNsPerMs));
  EXPECT_TRUE(adapter.book().empty());

  // 5. Stale REST snapshot (lastUpdateId=103 < first buffered U=105 - 1) must be rejected!
  auto stale_snap = adapter.on_rest_snapshot(
      R"({"lastUpdateId":103,"bids":[["64999.00","1.0"]],"asks":[["65005.00","1.0"]]})", 1040LL * kNsPerMs);
  EXPECT_TRUE(stale_snap.gap_detected);
  EXPECT_TRUE(stale_snap.needs_rest_snapshot);
  EXPECT_EQ(adapter.state(), BookState::Resyncing);

  // 6. Fresh REST snapshot (lastUpdateId=105) bridges [105..106] and restores Synced!
  auto ok_snap = adapter.on_rest_snapshot(
      R"({"lastUpdateId":105,"bids":[["64999.00","1.0"]],"asks":[["65005.00","1.0"]]})", 1050LL * kNsPerMs);
  EXPECT_TRUE(ok_snap.recovered_sync);
  EXPECT_EQ(adapter.state(), BookState::Synced);
  EXPECT_EQ(adapter.last_sequence(), 106ULL);
  EXPECT_EQ(adapter.book().best_bid()->price, 6500200LL);
}

XARB_TEST(CoinbaseSequenceGapAndHeartbeatLiveness) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  const auto& btc = *reg.get(0);
  CoinbaseAdapter adapter(btc, 500LL * kNsPerMs);

  // Delta before snapshot requests reconnect
  auto pre = adapter.on_ws_message(
      R"({"type":"l2update","product_id":"BTC-USDT","sequence":10,"time":"2026-11-24T00:00:01.000000Z","changes":[["buy","65000.00","1.0"]]})",
      1000LL * kNsPerMs);
  EXPECT_TRUE(pre.needs_ws_reconnect);
  EXPECT_NE(adapter.state(), BookState::Synced);

  // Snapshot syncs book
  auto snap = adapter.on_ws_message(
      R"({"type":"snapshot","product_id":"BTC-USDT","sequence":10,"time":"2026-11-24T00:00:01.100000Z","bids":[["65000.00","1.0"]],"asks":[["65002.00","1.0"]]})",
      1100LL * kNsPerMs);
  EXPECT_TRUE(snap.recovered_sync);
  EXPECT_EQ(adapter.state(), BookState::Synced);

  // Heartbeat at +400ms keeps book fresh at +700ms (past original 500ms window)
  auto hb = adapter.on_ws_message(
      R"({"type":"heartbeat","product_id":"BTC-USDT","sequence":11,"time":"2026-11-24T00:00:01.500000Z"})",
      1500LL * kNsPerMs);
  EXPECT_FALSE(hb.parse_error);
  EXPECT_TRUE(adapter.is_tradable(1800LL * kNsPerMs));

  // Sequence jump 11 -> 14 triggers Resyncing
  auto gap = adapter.on_ws_message(
      R"({"type":"l2update","product_id":"BTC-USDT","sequence":14,"time":"2026-11-24T00:00:01.900000Z","changes":[["buy","65001.00","2.0"]]})",
      1900LL * kNsPerMs);
  EXPECT_TRUE(gap.gap_detected);
  EXPECT_TRUE(gap.needs_ws_reconnect);
  EXPECT_EQ(adapter.state(), BookState::Resyncing);
}

XARB_TEST(OkxCrc32ChecksumAndPrevSeqIdRecovery) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  const auto& btc = *reg.get(0);
  OkxAdapter adapter(btc, 1000LL * kNsPerMs);

  // Build exact OKX checksum for bids=[65000.00:1.0] asks=[65002.00:2.0] -> "65000.00:1.0:65002.00:2.0"
  const int32_t snap_crc = Crc32::compute_signed("65000.00:1.0:65002.00:2.0");
  std::string snap_msg =
      "{\"arg\":{\"channel\":\"books\",\"instId\":\"BTC-USDT\"},\"action\":\"snapshot\",\"data\":[{\"asks\":[[\"65002.00\",\"2.0\",\"0\",\"1\"]],\"bids\":[[\"65000.00\",\"1.0\",\"0\",\"1\"]],\"ts\":\"1700000001000\",\"checksum\":" +
      std::to_string(snap_crc) + ",\"seqId\":500,\"prevSeqId\":-1}]}";
  auto s_act = adapter.on_ws_message(snap_msg, 1000LL * kNsPerMs);
  EXPECT_TRUE(s_act.recovered_sync);
  EXPECT_EQ(adapter.state(), BookState::Synced);
  EXPECT_EQ(adapter.compute_top25_checksum(), snap_crc);

  // Valid update with matching CRC32 ("65000.50:1.5:65002.00:2.0:65000.00:1.0")
  const int32_t upd_crc = Crc32::compute_signed("65000.50:1.5:65002.00:2.0:65000.00:1.0");
  std::string upd_msg =
      "{\"arg\":{\"channel\":\"books\",\"instId\":\"BTC-USDT\"},\"action\":\"update\",\"data\":[{\"asks\":[],\"bids\":[[\"65000.50\",\"1.5\",\"0\",\"1\"]],\"ts\":\"1700000001100\",\"checksum\":" +
      std::to_string(upd_crc) + ",\"seqId\":501,\"prevSeqId\":500}]}";
  auto u_act = adapter.on_ws_message(upd_msg, 1100LL * kNsPerMs);
  EXPECT_TRUE(u_act.book_updated);
  EXPECT_EQ(adapter.book().best_bid()->price, 6500050LL);

  // Corrupted checksum MUST trigger Resyncing and clear book!
  std::string bad_crc_msg =
      "{\"arg\":{\"channel\":\"books\",\"instId\":\"BTC-USDT\"},\"action\":\"update\",\"data\":[{\"asks\":[],\"bids\":[[\"65000.60\",\"1.0\",\"0\",\"1\"]],\"ts\":\"1700000001200\",\"checksum\":12345,\"seqId\":502,\"prevSeqId\":501}]}";
  auto bad_act = adapter.on_ws_message(bad_crc_msg, 1200LL * kNsPerMs);
  EXPECT_TRUE(bad_act.checksum_mismatch);
  EXPECT_TRUE(bad_act.needs_ws_reconnect);
  EXPECT_EQ(adapter.state(), BookState::Resyncing);
  EXPECT_EQ(adapter.metrics().checksum_mismatches, 1ULL);
}

XARB_TEST(StaleQuoteDetectionAndBackoff) {
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  const auto& btc = *reg.get(0);
  CoinbaseAdapter adapter(btc, 300LL * kNsPerMs);

  adapter.on_ws_message(
      R"({"type":"snapshot","product_id":"BTC-USDT","sequence":1,"time":"2026-11-24T00:00:01.000000Z","bids":[["65000.00","1.0"]],"asks":[["65002.00","1.0"]]})",
      1000LL * kNsPerMs);
  EXPECT_TRUE(adapter.is_tradable(1200LL * kNsPerMs));
  EXPECT_FALSE(adapter.is_tradable(1400LL * kNsPerMs));

  auto st = adapter.check_staleness(1400LL * kNsPerMs);
  EXPECT_TRUE(st.became_stale);
  EXPECT_EQ(adapter.state(), BookState::Stale);

  const TimestampNs b1 = adapter.next_reconnect_backoff_ns();
  const TimestampNs b2 = adapter.next_reconnect_backoff_ns();
  EXPECT_GT(b2, b1);
}

int main() { return xarb::test::run_all("test_venue_adapters"); }
