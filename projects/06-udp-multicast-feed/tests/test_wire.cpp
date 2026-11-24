#include "mdf/protocol.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace mdf;

MDF_TEST(PacketRoundTripMultiEvent) {
  PacketHeader hdr{};
  hdr.channel_id = 7;
  hdr.seq_num = 123456789ULL;
  hdr.send_ts_ns = 987654321012345ULL;
  hdr.flags = kFlagEndOfBatch;

  std::vector<MarketEvent> events = {
      {EventType::kAddOrder, 1001, 42, Side::kBid, 10125, 200},
      {EventType::kModifyOrder, 1001, 42, Side::kBid, 10130, 250},
      {EventType::kTrade, 1001, 42, Side::kBid, 10130, 50},
      {EventType::kCancelOrder, 1001, 42, Side::kBid, 0, 100},
      {EventType::kClearBook, 1001, 0, Side::kBid, 0, 0},
  };

  std::vector<uint8_t> wire;
  EXPECT_TRUE(encode_packet(hdr, events, wire));

  // Verify big-endian magic "MDF1".
  EXPECT_EQ(wire[0], static_cast<uint8_t>('M'));
  EXPECT_EQ(wire[1], static_cast<uint8_t>('D'));
  EXPECT_EQ(wire[2], static_cast<uint8_t>('F'));
  EXPECT_EQ(wire[3], static_cast<uint8_t>('1'));

  DecodedPacket decoded{};
  EXPECT_EQ(decode_packet(wire, decoded), DecodeStatus::kOk);
  EXPECT_EQ(decoded.header.channel_id, 7U);
  EXPECT_EQ(decoded.header.seq_num, 123456789ULL);
  EXPECT_EQ(decoded.header.send_ts_ns, 987654321012345ULL);
  EXPECT_EQ(decoded.header.flags, kFlagEndOfBatch);
  EXPECT_EQ(decoded.header.msg_count, 5U);
  EXPECT_EQ(decoded.events.size(), events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(decoded.events[i].type, events[i].type);
    EXPECT_EQ(decoded.events[i].instrument_id, events[i].instrument_id);
    EXPECT_EQ(decoded.events[i].order_id, events[i].order_id);
    if (events[i].type == EventType::kAddOrder) {
      EXPECT_EQ(decoded.events[i].side, events[i].side);
    }
    if (events[i].type != EventType::kCancelOrder && events[i].type != EventType::kClearBook) {
      EXPECT_EQ(decoded.events[i].price_ticks, events[i].price_ticks);
    }
    if (events[i].type != EventType::kClearBook) {
      EXPECT_EQ(decoded.events[i].qty, events[i].qty);
    }
  }
}

MDF_TEST(HeartbeatRoundTrip) {
  std::vector<uint8_t> wire;
  EXPECT_TRUE(encode_heartbeat(3, 999, 55555, wire));
  EXPECT_EQ(wire.size(), kPacketHeaderSize);

  DecodedPacket decoded{};
  EXPECT_EQ(decode_packet(wire, decoded), DecodeStatus::kOk);
  EXPECT_EQ(decoded.header.channel_id, 3U);
  EXPECT_EQ(decoded.header.seq_num, 999ULL);
  EXPECT_TRUE((decoded.header.flags & kFlagHeartbeat) != 0U);
  EXPECT_EQ(decoded.header.msg_count, 0U);
  EXPECT_TRUE(decoded.events.empty());
}

MDF_TEST(PacketNegativeCases) {
  PacketHeader hdr{};
  hdr.channel_id = 1;
  hdr.seq_num = 10;
  std::vector<MarketEvent> events = {{EventType::kAddOrder, 1001, 1, Side::kAsk, 5000, 100}};

  std::vector<uint8_t> wire;
  EXPECT_TRUE(encode_packet(hdr, events, wire));

  DecodedPacket out{};

  // Truncated header.
  EXPECT_EQ(decode_packet(std::span<const uint8_t>(wire.data(), 10), out),
            DecodeStatus::kTruncated);

  // Bad magic.
  auto bad_magic = wire;
  bad_magic[0] = 'X';
  EXPECT_EQ(decode_packet(bad_magic, out), DecodeStatus::kBadMagic);

  // Unsupported version.
  auto bad_ver = wire;
  bad_ver[5] = 99;
  EXPECT_EQ(decode_packet(bad_ver, out), DecodeStatus::kUnsupportedVersion);

  // Length mismatch (trailing garbage).
  auto extra = wire;
  extra.push_back(0xAA);
  EXPECT_EQ(decode_packet(extra, out), DecodeStatus::kLengthMismatch);

  // Bit flip in header (seq_num byte) -> CRC32 failure.
  auto corrupt_hdr = wire;
  corrupt_hdr[20] ^= 0x01U;
  EXPECT_EQ(decode_packet(corrupt_hdr, out), DecodeStatus::kBadChecksum);

  // Bit flip in payload -> CRC32 failure.
  auto corrupt_body = wire;
  corrupt_body.back() ^= 0x80U;
  EXPECT_EQ(decode_packet(corrupt_body, out), DecodeStatus::kBadChecksum);

  // Oversized payload exceeding MTU budget must fail to encode.
  std::vector<MarketEvent> huge(100, events[0]);
  std::vector<uint8_t> huge_wire;
  EXPECT_FALSE(encode_packet(hdr, huge, huge_wire));
}

MDF_TEST(RecoveryFramesRoundTripAndChecksum) {
  // Retransmit request.
  RetransmitRequest rreq{4, 100, 150};
  std::vector<uint8_t> frame;
  EXPECT_TRUE(encode_retransmit_request(rreq, frame));

  RecoveryFrameHeader fhdr{};
  EXPECT_EQ(decode_recovery_frame_header(frame, fhdr), DecodeStatus::kOk);
  EXPECT_EQ(fhdr.type, RecoveryMsgType::kRetransmitReq);
  RetransmitRequest rreq_out{};
  EXPECT_EQ(decode_retransmit_request(
                std::span<const uint8_t>(frame).subspan(kRecoveryFrameHeaderSize), rreq_out),
            DecodeStatus::kOk);
  EXPECT_EQ(rreq_out, rreq);

  // Retransmit response with 2 packets.
  std::vector<uint8_t> p1;
  std::vector<uint8_t> p2;
  EXPECT_TRUE(encode_heartbeat(4, 100, 1, p1));
  EXPECT_TRUE(encode_heartbeat(4, 101, 2, p2));
  RetransmitResponse rresp{4, RecoveryStatus::kOk, 50, 200, 100, {p1, p2}};
  EXPECT_TRUE(encode_retransmit_response(rresp, frame));
  EXPECT_EQ(decode_recovery_frame_header(frame, fhdr), DecodeStatus::kOk);
  EXPECT_EQ(fhdr.type, RecoveryMsgType::kRetransmitResp);
  RetransmitResponse rresp_out{};
  EXPECT_EQ(decode_retransmit_response(
                std::span<const uint8_t>(frame).subspan(kRecoveryFrameHeaderSize), rresp_out),
            DecodeStatus::kOk);
  EXPECT_EQ(rresp_out, rresp);

  // Snapshot response + CRC check.
  SnapshotResponse sresp{};
  sresp.status = RecoveryStatus::kOk;
  sresp.snapshot.channel_id = 4;
  sresp.snapshot.as_of_seq = 500;
  sresp.snapshot.checkpoint_id = 3;
  sresp.snapshot.orders = {
      {1001, 11, Side::kBid, 9950, 100},
      {1002, 12, Side::kAsk, 10050, 300},
  };
  sresp.snapshot.instrument_stats = {{1001, 9955, 25, 10, 450}};

  EXPECT_TRUE(encode_snapshot_response(sresp, frame));
  EXPECT_EQ(decode_recovery_frame_header(frame, fhdr), DecodeStatus::kOk);
  EXPECT_EQ(fhdr.type, RecoveryMsgType::kSnapshotResp);
  SnapshotResponse sresp_out{};
  EXPECT_EQ(decode_snapshot_response(
                std::span<const uint8_t>(frame).subspan(kRecoveryFrameHeaderSize), sresp_out),
            DecodeStatus::kOk);
  EXPECT_EQ(sresp_out, sresp);

  // Flip a byte in snapshot payload -> must detect bad checksum.
  frame[kRecoveryFrameHeaderSize + 10] ^= 0x04U;
  EXPECT_EQ(decode_snapshot_response(
                std::span<const uint8_t>(frame).subspan(kRecoveryFrameHeaderSize), sresp_out),
            DecodeStatus::kBadChecksum);
}

MDF_TEST_MAIN()
