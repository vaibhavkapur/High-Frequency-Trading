#include "mdf/channel_receiver.hpp"
#include "mdf/exchange_simulator.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace mdf;

namespace {

std::vector<uint8_t> make_pkt(uint16_t ch, uint64_t seq, const MarketEvent& ev) {
  PacketHeader hdr{};
  hdr.channel_id = ch;
  hdr.seq_num = seq;
  hdr.send_ts_ns = seq * 1000;
  std::vector<uint8_t> out;
  encode_packet(hdr, std::span<const MarketEvent>(&ev, 1), out);
  return out;
}

}  // namespace

MDF_TEST(DroppedPacketTriggersGapAlarmNotSilentCorruption) {
  ChannelReceiverConfig cfg{};
  cfg.channel_id = 1;
  cfg.reorder_window_packets = 10;

  ChannelReceiver rx(cfg, nullptr);
  std::vector<GapAlarm> alarms;
  rx.set_gap_alarm_callback([&](const GapAlarm& a) { alarms.push_back(a); });

  const auto p1 = make_pkt(1, 1, {EventType::kAddOrder, 1001, 1, Side::kBid, 1000, 100});
  const auto p3 = make_pkt(1, 3, {EventType::kCancelOrder, 1001, 1, Side::kBid, 0, 100});

  EXPECT_EQ(rx.on_datagram(p1, 1000), DecodeStatus::kOk);
  EXPECT_EQ(rx.state(), ReceiverState::kLive);
  EXPECT_EQ(rx.market_state().last_applied_seq(), 1ULL);
  EXPECT_EQ(rx.market_state().total_active_orders(), 1U);

  // Drop seq 2 and deliver seq 3: MUST alarm and NOT apply seq 3 (which would cancel order 1).
  EXPECT_EQ(rx.on_datagram(p3, 2000), DecodeStatus::kOk);
  EXPECT_EQ(rx.state(), ReceiverState::kGapWait);
  EXPECT_EQ(alarms.size(), 1U);
  EXPECT_EQ(alarms[0].expected_seq, 2ULL);
  EXPECT_EQ(alarms[0].received_seq, 3ULL);
  EXPECT_EQ(alarms[0].missing_count, 1ULL);
  EXPECT_EQ(rx.stats().gaps_detected, 1ULL);
  EXPECT_EQ(rx.market_state().last_applied_seq(), 1ULL);
  EXPECT_EQ(rx.market_state().total_active_orders(), 1U);
  EXPECT_EQ(rx.buffered_packets(), 1U);
}

MDF_TEST(OutOfOrderAndDuplicateHandling) {
  ChannelReceiverConfig cfg{};
  cfg.channel_id = 1;
  cfg.reorder_window_packets = 5;

  ChannelReceiver rx(cfg, nullptr);
  ChannelMarketState ref(1);

  std::vector<MarketEvent> evs = {
      {EventType::kAddOrder, 1001, 1, Side::kBid, 1000, 100},
      {EventType::kAddOrder, 1001, 2, Side::kAsk, 1010, 50},
      {EventType::kModifyOrder, 1001, 1, Side::kBid, 1002, 120},
      {EventType::kTrade, 1001, 1, Side::kBid, 1002, 20},
  };
  std::vector<std::vector<uint8_t>> pkts;
  for (std::size_t i = 0; i < evs.size(); ++i) {
    ref.apply_packet(i + 1, std::span<const MarketEvent>(&evs[i], 1));
    pkts.push_back(make_pkt(1, i + 1, evs[i]));
  }

  // Deliver: 1, 1 (dup), 3 (ooo), 3 (dup buffered), 4 (ooo), 2 (fills gap!), 2 (dup).
  EXPECT_EQ(rx.on_datagram(pkts[0], 1000), DecodeStatus::kOk);
  EXPECT_EQ(rx.on_datagram(pkts[0], 1100), DecodeStatus::kOk);
  EXPECT_EQ(rx.on_datagram(pkts[2], 1200), DecodeStatus::kOk);
  EXPECT_EQ(rx.on_datagram(pkts[2], 1300), DecodeStatus::kOk);
  EXPECT_EQ(rx.on_datagram(pkts[3], 1400), DecodeStatus::kOk);
  EXPECT_EQ(rx.state(), ReceiverState::kGapWait);

  // Arriving packet 2 closes the hole and drains 3 & 4 without needing TCP!
  EXPECT_EQ(rx.on_datagram(pkts[1], 1500), DecodeStatus::kOk);
  EXPECT_EQ(rx.state(), ReceiverState::kLive);
  EXPECT_EQ(rx.on_datagram(pkts[1], 1600), DecodeStatus::kOk);

  EXPECT_EQ(rx.stats().duplicate_packets, 3ULL);
  EXPECT_EQ(rx.stats().reordered_recovered_packets, 2ULL);
  EXPECT_EQ(rx.stats().retransmit_requests, 0ULL);
  EXPECT_TRUE(rx.market_state() == ref);
  EXPECT_EQ(rx.market_state().state_hash(), ref.state_hash());
}

MDF_TEST(CorruptPacketRejectedAndBoundedBufferEnforced) {
  ChannelReceiverConfig cfg{};
  cfg.channel_id = 1;
  cfg.reorder_window_packets = 100;
  cfg.max_reorder_buffer = 3;

  ChannelReceiver rx(cfg, nullptr);
  auto p1 = make_pkt(1, 1, {EventType::kAddOrder, 1001, 1, Side::kBid, 1000, 100});
  auto corrupt = p1;
  corrupt.back() ^= 0xFFU;

  EXPECT_EQ(rx.on_datagram(corrupt, 1000), DecodeStatus::kBadChecksum);
  EXPECT_EQ(rx.stats().checksum_errors, 1ULL);
  EXPECT_EQ(rx.market_state().total_active_orders(), 0U);

  // Deliver seq 2, 3, 4, 5, 6 without seq 1 -> max_reorder_buffer=3 drops 5 and 6.
  for (uint64_t s = 2; s <= 6; ++s) {
    auto p = make_pkt(1, s, {EventType::kAddOrder, 1001, s, Side::kBid, 1000, 10});
    EXPECT_EQ(rx.on_datagram(p, s * 1000), DecodeStatus::kOk);
  }
  EXPECT_EQ(rx.buffered_packets(), 3U);
  EXPECT_EQ(rx.stats().buffer_overflow_drops, 2ULL);
}

MDF_TEST(SnapshotAtSeqNDiscardsStaleIncrementalsAndAppliesOnlyBeyondN) {
  // Build authoritative sequence 1..10.
  ExchangeSimulator sim({99, {1001, 1002}, 10000, 16});
  ChannelMarketState ref(1);
  std::vector<std::vector<uint8_t>> pkts;
  SnapshotData snap_at_6{};

  for (uint64_t seq = 1; seq <= 10; ++seq) {
    auto batch = sim.next_batch(3);
    ref.apply_packet(seq, batch);

    PacketHeader hdr{};
    hdr.channel_id = 1;
    hdr.seq_num = seq;
    hdr.send_ts_ns = seq * 1000;
    std::vector<uint8_t> wire;
    EXPECT_TRUE(encode_packet(hdr, batch, wire));
    pkts.push_back(std::move(wire));

    if (seq == 6) {
      snap_at_6 = ref.export_snapshot(1);
    }
  }

  // Receiver only saw seq 1, then got buffered packets 4, 5, 6 (stale relative to snap@6),
  // plus 7, 8, 9, 10 (strictly beyond 6).
  ChannelReceiverConfig cfg{};
  cfg.channel_id = 1;
  cfg.reorder_window_packets = 100;
  ChannelReceiver rx(cfg, nullptr);

  EXPECT_EQ(rx.on_datagram(pkts[0], 1000), DecodeStatus::kOk);  // seq 1
  for (uint64_t s = 4; s <= 10; ++s) {
    EXPECT_EQ(rx.on_datagram(pkts[s - 1], s * 1000), DecodeStatus::kOk);
  }
  EXPECT_EQ(rx.buffered_packets(), 7U);  // seqs 4,5,6,7,8,9,10

  // Apply snapshot at N=6: MUST discard 4,5,6 and apply ONLY 7,8,9,10!
  rx.apply_snapshot(snap_at_6);

  EXPECT_EQ(rx.stats().snapshots_applied, 1ULL);
  EXPECT_EQ(rx.stats().stale_incrementals_discarded, 3ULL);  // 4, 5, 6 discarded!
  EXPECT_EQ(rx.buffered_packets(), 0U);
  EXPECT_EQ(rx.expected_seq(), 11ULL);
  EXPECT_TRUE(rx.market_state() == ref);
  EXPECT_EQ(rx.market_state().state_hash(), ref.state_hash());
}

MDF_TEST(HeartbeatDetectsTailPacketLoss) {
  RecoveryStore store({64, 32, 20});
  store.register_channel(1);
  InMemoryRecoveryClient client(store);

  ChannelMarketState ref(1);
  std::vector<std::vector<uint8_t>> pkts;
  for (uint64_t seq = 1; seq <= 5; ++seq) {
    MarketEvent ev{EventType::kAddOrder, 1001, seq, Side::kBid, 1000 + static_cast<int64_t>(seq),
                   100};
    ref.apply_packet(seq, std::span<const MarketEvent>(&ev, 1));
    auto wire = make_pkt(1, seq, ev);
    store.record_packet(1, seq, wire, ref);
    pkts.push_back(std::move(wire));
  }

  ChannelReceiver rx({1, 4, 2000, 64, 32, true}, &client);
  // Deliver 1..4, drop tail packet 5!
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(rx.on_datagram(pkts[i], (i + 1) * 1000), DecodeStatus::kOk);
  }
  EXPECT_EQ(rx.market_state().last_applied_seq(), 4ULL);

  // Heartbeat announces seq 5 -> receiver detects missing tail packet 5 and recovers it!
  std::vector<uint8_t> hb;
  EXPECT_TRUE(encode_heartbeat(1, 5, 6000, hb));
  EXPECT_EQ(rx.on_datagram(hb, 6000), DecodeStatus::kOk);

  EXPECT_EQ(rx.state(), ReceiverState::kLive);
  EXPECT_EQ(rx.stats().heartbeats_received, 1ULL);
  EXPECT_EQ(rx.stats().retransmit_packets_recovered, 1ULL);
  EXPECT_TRUE(rx.market_state() == ref);
  EXPECT_EQ(rx.market_state().state_hash(), ref.state_hash());
}

MDF_TEST_MAIN()
