#include "mdf/channel_receiver.hpp"
#include "mdf/crc32.hpp"
#include "mdf/exchange_simulator.hpp"
#include "mdf/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace mdf;

namespace {

void exercise_all_decoders(std::span<const uint8_t> data) {
  DecodedPacket pkt{};
  (void)decode_packet(data, pkt);

  RecoveryFrameHeader fhdr{};
  if (decode_recovery_frame_header(data, fhdr) == DecodeStatus::kOk &&
      data.size() >= kRecoveryFrameHeaderSize) {
    const auto payload = data.subspan(kRecoveryFrameHeaderSize);
    RetransmitRequest rreq{};
    (void)decode_retransmit_request(payload, rreq);
    RetransmitResponse rresp{};
    (void)decode_retransmit_response(payload, rresp);
    SnapshotRequest sreq{};
    (void)decode_snapshot_request(payload, sreq);
    SnapshotResponse sresp{};
    (void)decode_snapshot_response(payload, sresp);
  } else {
    RetransmitRequest rreq{};
    (void)decode_retransmit_request(data, rreq);
    RetransmitResponse rresp{};
    (void)decode_retransmit_response(data, rresp);
    SnapshotRequest sreq{};
    (void)decode_snapshot_request(data, sreq);
    SnapshotResponse sresp{};
    (void)decode_snapshot_response(data, sresp);
  }
}

// Re-sign valid CRC32 on a mutated MDF1 datagram so fuzzing penetrates past the checksum check
// into event-framing and field validation code paths!
void resign_mdf1_crc(std::vector<uint8_t>& buf) {
  if (buf.size() < kPacketHeaderSize) return;
  buf[0] = 'M';
  buf[1] = 'D';
  buf[2] = 'F';
  buf[3] = '1';
  buf[4] = 0;
  buf[5] = 1;
  const uint32_t payload_len = static_cast<uint32_t>(buf.size() - kPacketHeaderSize);
  buf[12] = static_cast<uint8_t>((payload_len >> 24U) & 0xFFU);
  buf[13] = static_cast<uint8_t>((payload_len >> 16U) & 0xFFU);
  buf[14] = static_cast<uint8_t>((payload_len >> 8U) & 0xFFU);
  buf[15] = static_cast<uint8_t>(payload_len & 0xFFU);
  buf[32] = buf[33] = buf[34] = buf[35] = 0;
  const uint32_t sum = crc32(buf);
  buf[32] = static_cast<uint8_t>((sum >> 24U) & 0xFFU);
  buf[33] = static_cast<uint8_t>((sum >> 16U) & 0xFFU);
  buf[34] = static_cast<uint8_t>((sum >> 8U) & 0xFFU);
  buf[35] = static_cast<uint8_t>(sum & 0xFFU);
}

}  // namespace

#ifdef MDF_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  std::span<const uint8_t> raw(data, size);
  exercise_all_decoders(raw);

  if (size >= kPacketHeaderSize && size <= kMaxUdpDatagramSize) {
    std::vector<uint8_t> signed_buf(data, data + size);
    resign_mdf1_crc(signed_buf);
    DecodedPacket pkt{};
    (void)decode_packet(signed_buf, pkt);
  }
  return 0;
}

#else

#include "test_framework.hpp"

MDF_TEST(FuzzBinaryDecodersRandomAndMutatedInputs) {
  std::mt19937_64 rng(0xCAFEBABEULL);
  std::uniform_int_distribution<std::size_t> len_dist(0, 512);
  std::uniform_int_distribution<int> byte_dist(0, 255);

  // Seed corpus of valid encoded packets and recovery frames.
  std::vector<std::vector<uint8_t>> corpus;
  {
    PacketHeader hdr{};
    hdr.channel_id = 1;
    hdr.seq_num = 1;
    std::vector<MarketEvent> evs = {
        {EventType::kAddOrder, 1001, 1, Side::kBid, 1000, 100},
        {EventType::kModifyOrder, 1001, 1, Side::kBid, 1005, 120},
        {EventType::kTrade, 1001, 1, Side::kBid, 1005, 20},
        {EventType::kCancelOrder, 1001, 1, Side::kBid, 0, 50},
        {EventType::kClearBook, 1001, 0, Side::kBid, 0, 0},
    };
    std::vector<uint8_t> w;
    EXPECT_TRUE(encode_packet(hdr, evs, w));
    corpus.push_back(w);

    SnapshotResponse sr{};
    sr.status = RecoveryStatus::kOk;
    sr.snapshot.channel_id = 1;
    sr.snapshot.as_of_seq = 10;
    sr.snapshot.orders = {{1001, 1, Side::kBid, 1000, 100}};
    sr.snapshot.instrument_stats = {{1001, 1000, 10, 1, 10}};
    EXPECT_TRUE(encode_snapshot_response(sr, w));
    corpus.push_back(w);
  }

  // 25,000 completely random inputs + 25,000 mutated CRC-resigned inputs.
  for (int i = 0; i < 25000; ++i) {
    const std::size_t len = len_dist(rng);
    std::vector<uint8_t> buf(len);
    for (auto& b : buf) b = static_cast<uint8_t>(byte_dist(rng));
    exercise_all_decoders(buf);

    if (len >= kPacketHeaderSize) {
      resign_mdf1_crc(buf);
      DecodedPacket pkt{};
      (void)decode_packet(buf, pkt);
    }
  }

  for (int i = 0; i < 25000; ++i) {
    std::vector<uint8_t> buf = corpus[static_cast<std::size_t>(i) % corpus.size()];
    const int flips = 1 + (i % 6);
    for (int f = 0; f < flips && !buf.empty(); ++f) {
      const std::size_t idx = rng() % buf.size();
      buf[idx] ^= static_cast<uint8_t>(1U << (rng() % 8));
    }
    if ((i % 3) == 0 && buf.size() > 4) {
      buf.resize(rng() % buf.size());
    }
    exercise_all_decoders(buf);
    if ((i % 2) == 0 && buf.size() >= kPacketHeaderSize && buf.size() <= kMaxUdpDatagramSize) {
      resign_mdf1_crc(buf);
      DecodedPacket pkt{};
      (void)decode_packet(buf, pkt);
    }
  }

  EXPECT_TRUE(true);
}

MDF_TEST(PropertyRandomizedFaultSchedulesAlwaysConverge) {
  std::mt19937_64 meta_rng(987654321ULL);
  std::uniform_real_distribution<double> prob_drop(0.0, 0.20);
  std::uniform_real_distribution<double> prob_reorder(0.0, 0.30);
  std::uniform_real_distribution<double> prob_dup(0.0, 0.20);
  std::uniform_real_distribution<double> prob_corrupt(0.0, 0.10);

  for (int trial = 0; trial < 40; ++trial) {
    const uint64_t seed = meta_rng();
    const std::size_t ring_cap = 32 + (meta_rng() % 96);
    const uint64_t ckpt_int = 16 + (meta_rng() % 48);

    RecoveryStore store({ring_cap, 32, ckpt_int, true});
    InMemoryRecoveryClient recovery(store);
    MulticastFeedPublisher pub(store);
    pub.register_channel(1);

    ChannelReceiver rx({1, 3, 2000, 256, 32, false}, &recovery);

    FaultConfig fcfg{};
    fcfg.seed = seed;
    fcfg.drop_prob = prob_drop(meta_rng);
    fcfg.reorder_prob = prob_reorder(meta_rng);
    fcfg.max_reorder_delay = 1 + static_cast<uint32_t>(meta_rng() % 8);
    fcfg.duplicate_prob = prob_dup(meta_rng);
    fcfg.corrupt_prob = prob_corrupt(meta_rng);
    if ((trial % 2) == 0) {
      // Half the trials also inject an unrecoverable disconnect forcing snapshot recovery.
      fcfg.drop_ranges = {{40, 40 + ring_cap + 30}};
    }
    FaultInjector fault(fcfg);

    uint64_t ts = 1'000'000;
    pub.set_custom_sink([&](std::span<const uint8_t> dgram) {
      ts += 1000;
      for (const auto& item : fault.process(dgram)) {
        (void)rx.on_datagram(item.bytes, ts);
      }
      return true;
    });

    ExchangeSimulator sim({seed, {1001, 1002, 1003}, 10000, 24});
    const int total_pkts = 250;
    for (int p = 0; p < total_pkts; ++p) {
      const std::size_t batch_sz = 1 + (meta_rng() % 6);
      EXPECT_GT(pub.publish_events(1, sim.next_batch(batch_sz)), 0ULL);
    }
    for (const auto& item : fault.flush()) {
      ts += 1000;
      (void)rx.on_datagram(item.bytes, ts);
    }
    EXPECT_TRUE(pub.publish_heartbeat(1));
    EXPECT_TRUE(rx.flush_recovery(ts + 5'000'000));

    EXPECT_EQ(rx.state(), ReceiverState::kLive);
    EXPECT_TRUE(rx.market_state() == pub.authoritative_state(1));
    EXPECT_EQ(rx.market_state().state_hash(), pub.authoritative_state(1).state_hash());
  }
}

MDF_TEST_MAIN()

#endif
