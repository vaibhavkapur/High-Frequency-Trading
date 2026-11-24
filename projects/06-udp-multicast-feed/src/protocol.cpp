#include "mdf/protocol.hpp"

#include <chrono>
#include <cstring>
#include <limits>

#include "mdf/crc32.hpp"

namespace mdf {

namespace {

inline void write_u16_be(uint8_t* dst, uint16_t v) noexcept {
  dst[0] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
  dst[1] = static_cast<uint8_t>(v & 0xFFU);
}

inline void write_u32_be(uint8_t* dst, uint32_t v) noexcept {
  dst[0] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
  dst[1] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
  dst[2] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
  dst[3] = static_cast<uint8_t>(v & 0xFFU);
}

inline void write_u64_be(uint8_t* dst, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<uint8_t>((v >> (56U - static_cast<unsigned>(i) * 8U)) & 0xFFU);
  }
}

inline void write_i64_be(uint8_t* dst, int64_t v) noexcept {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  write_u64_be(dst, bits);
}

inline uint16_t read_u16_be(const uint8_t* src) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8U) |
                               static_cast<uint16_t>(src[1]));
}

inline uint32_t read_u32_be(const uint8_t* src) noexcept {
  return (static_cast<uint32_t>(src[0]) << 24U) | (static_cast<uint32_t>(src[1]) << 16U) |
         (static_cast<uint32_t>(src[2]) << 8U) | static_cast<uint32_t>(src[3]);
}

inline uint64_t read_u64_be(const uint8_t* src) noexcept {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8U) | static_cast<uint64_t>(src[i]);
  }
  return v;
}

inline int64_t read_i64_be(const uint8_t* src) noexcept {
  uint64_t bits = read_u64_be(src);
  int64_t v = 0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

inline void append_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

inline void append_u16_be(std::vector<uint8_t>& buf, uint16_t v) {
  const std::size_t pos = buf.size();
  buf.resize(pos + 2);
  write_u16_be(buf.data() + pos, v);
}

inline void append_u32_be(std::vector<uint8_t>& buf, uint32_t v) {
  const std::size_t pos = buf.size();
  buf.resize(pos + 4);
  write_u32_be(buf.data() + pos, v);
}

inline void append_u64_be(std::vector<uint8_t>& buf, uint64_t v) {
  const std::size_t pos = buf.size();
  buf.resize(pos + 8);
  write_u64_be(buf.data() + pos, v);
}

inline void append_i64_be(std::vector<uint8_t>& buf, int64_t v) {
  const std::size_t pos = buf.size();
  buf.resize(pos + 8);
  write_i64_be(buf.data() + pos, v);
}

// Event record wire sizes (excluding the 2-byte length prefix):
//   [1B type] + body
//   AddOrder:    1 + 4(inst) + 8(oid) + 1(side) + 8(price) + 4(qty) = 26
//   ModifyOrder: 1 + 4(inst) + 8(oid) + 8(price) + 4(qty)           = 25
//   CancelOrder: 1 + 4(inst) + 8(oid) + 4(qty)                      = 17
//   Trade:       1 + 4(inst) + 8(oid) + 8(price) + 4(qty)           = 25
//   ClearBook:   1 + 4(inst)                                        = 5
bool append_event(const MarketEvent& ev, std::vector<uint8_t>& buf) {
  switch (ev.type) {
    case EventType::kAddOrder:
      if (ev.side != Side::kBid && ev.side != Side::kAsk) return false;
      append_u16_be(buf, 26);
      append_u8(buf, static_cast<uint8_t>(ev.type));
      append_u32_be(buf, ev.instrument_id);
      append_u64_be(buf, ev.order_id);
      append_u8(buf, static_cast<uint8_t>(ev.side));
      append_i64_be(buf, ev.price_ticks);
      append_u32_be(buf, ev.qty);
      return true;
    case EventType::kModifyOrder:
      append_u16_be(buf, 25);
      append_u8(buf, static_cast<uint8_t>(ev.type));
      append_u32_be(buf, ev.instrument_id);
      append_u64_be(buf, ev.order_id);
      append_i64_be(buf, ev.price_ticks);
      append_u32_be(buf, ev.qty);
      return true;
    case EventType::kCancelOrder:
      append_u16_be(buf, 17);
      append_u8(buf, static_cast<uint8_t>(ev.type));
      append_u32_be(buf, ev.instrument_id);
      append_u64_be(buf, ev.order_id);
      append_u32_be(buf, ev.qty);
      return true;
    case EventType::kTrade:
      append_u16_be(buf, 25);
      append_u8(buf, static_cast<uint8_t>(ev.type));
      append_u32_be(buf, ev.instrument_id);
      append_u64_be(buf, ev.order_id);
      append_i64_be(buf, ev.price_ticks);
      append_u32_be(buf, ev.qty);
      return true;
    case EventType::kClearBook:
      append_u16_be(buf, 5);
      append_u8(buf, static_cast<uint8_t>(ev.type));
      append_u32_be(buf, ev.instrument_id);
      return true;
  }
  return false;
}

bool write_recovery_frame(RecoveryMsgType type,
                          std::span<const uint8_t> payload,
                          std::vector<uint8_t>& out_frame) {
  if (payload.size() > kMaxRecoveryFramePayload) {
    return false;
  }
  out_frame.clear();
  out_frame.reserve(kRecoveryFrameHeaderSize + payload.size());
  append_u32_be(out_frame, kRecoveryMagic);
  append_u16_be(out_frame, kProtocolVersion);
  append_u16_be(out_frame, static_cast<uint16_t>(type));
  append_u32_be(out_frame, static_cast<uint32_t>(payload.size()));
  out_frame.insert(out_frame.end(), payload.begin(), payload.end());
  return true;
}

class Cursor {
 public:
  explicit Cursor(std::span<const uint8_t> data) : data_(data) {}

  std::size_t remaining() const noexcept { return data_.size() - pos_; }
  std::size_t pos() const noexcept { return pos_; }

  bool read_u8(uint8_t& out) noexcept {
    if (remaining() < 1) return false;
    out = data_[pos_++];
    return true;
  }

  bool read_u16(uint16_t& out) noexcept {
    if (remaining() < 2) return false;
    out = read_u16_be(data_.data() + pos_);
    pos_ += 2;
    return true;
  }

  bool read_u32(uint32_t& out) noexcept {
    if (remaining() < 4) return false;
    out = read_u32_be(data_.data() + pos_);
    pos_ += 4;
    return true;
  }

  bool read_u64(uint64_t& out) noexcept {
    if (remaining() < 8) return false;
    out = read_u64_be(data_.data() + pos_);
    pos_ += 8;
    return true;
  }

  bool read_i64(int64_t& out) noexcept {
    if (remaining() < 8) return false;
    out = read_i64_be(data_.data() + pos_);
    pos_ += 8;
    return true;
  }

  bool read_bytes(std::size_t n, std::span<const uint8_t>& out) noexcept {
    if (remaining() < n) return false;
    out = data_.subspan(pos_, n);
    pos_ += n;
    return true;
  }

 private:
  std::span<const uint8_t> data_;
  std::size_t pos_ = 0;
};

}  // namespace

const char* decode_status_name(DecodeStatus status) noexcept {
  switch (status) {
    case DecodeStatus::kOk:
      return "OK";
    case DecodeStatus::kTruncated:
      return "TRUNCATED";
    case DecodeStatus::kBadMagic:
      return "BAD_MAGIC";
    case DecodeStatus::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case DecodeStatus::kLengthMismatch:
      return "LENGTH_MISMATCH";
    case DecodeStatus::kBadChecksum:
      return "BAD_CHECKSUM";
    case DecodeStatus::kBadEventCount:
      return "BAD_EVENT_COUNT";
    case DecodeStatus::kBadEventType:
      return "BAD_EVENT_TYPE";
    case DecodeStatus::kInvalidField:
      return "INVALID_FIELD";
    case DecodeStatus::kPayloadTooLarge:
      return "PAYLOAD_TOO_LARGE";
  }
  return "UNKNOWN";
}

bool encode_packet(const PacketHeader& header_in,
                   std::span<const MarketEvent> events,
                   std::vector<uint8_t>& out) {
  if (events.size() > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  out.clear();
  out.resize(kPacketHeaderSize, 0);

  for (const auto& ev : events) {
    if (!append_event(ev, out)) {
      out.clear();
      return false;
    }
  }

  const std::size_t payload_len = out.size() - kPacketHeaderSize;
  if (payload_len > kMaxPacketPayloadSize) {
    out.clear();
    return false;
  }

  write_u32_be(out.data() + 0, kPacketMagic);
  write_u16_be(out.data() + 4, kProtocolVersion);
  write_u16_be(out.data() + 6, header_in.flags);
  write_u16_be(out.data() + 8, header_in.channel_id);
  write_u16_be(out.data() + 10, static_cast<uint16_t>(events.size()));
  write_u32_be(out.data() + 12, static_cast<uint32_t>(payload_len));
  write_u64_be(out.data() + 16, header_in.seq_num);
  write_u64_be(out.data() + 24, header_in.send_ts_ns);
  write_u32_be(out.data() + 32, 0U);  // Zero checksum slot before computing CRC32.

  const uint32_t sum = crc32(std::span<const uint8_t>(out.data(), out.size()));
  write_u32_be(out.data() + 32, sum);
  return true;
}

bool encode_heartbeat(uint16_t channel_id,
                      uint64_t last_published_seq,
                      uint64_t send_ts_ns,
                      std::vector<uint8_t>& out) {
  PacketHeader hdr{};
  hdr.flags = kFlagHeartbeat;
  hdr.channel_id = channel_id;
  hdr.seq_num = last_published_seq;
  hdr.send_ts_ns = send_ts_ns;
  return encode_packet(hdr, {}, out);
}

DecodeStatus decode_packet_header(std::span<const uint8_t> datagram, PacketHeader& out_header) {
  if (datagram.size() < kPacketHeaderSize) {
    return DecodeStatus::kTruncated;
  }
  if (datagram.size() > kMaxUdpDatagramSize) {
    return DecodeStatus::kPayloadTooLarge;
  }

  PacketHeader hdr{};
  hdr.magic = read_u32_be(datagram.data() + 0);
  if (hdr.magic != kPacketMagic) {
    return DecodeStatus::kBadMagic;
  }
  hdr.version = read_u16_be(datagram.data() + 4);
  if (hdr.version != kProtocolVersion) {
    return DecodeStatus::kUnsupportedVersion;
  }
  hdr.flags = read_u16_be(datagram.data() + 6);
  hdr.channel_id = read_u16_be(datagram.data() + 8);
  hdr.msg_count = read_u16_be(datagram.data() + 10);
  hdr.payload_len = read_u32_be(datagram.data() + 12);
  hdr.seq_num = read_u64_be(datagram.data() + 16);
  hdr.send_ts_ns = read_u64_be(datagram.data() + 24);
  hdr.checksum = read_u32_be(datagram.data() + 32);

  if (hdr.payload_len > kMaxPacketPayloadSize) {
    return DecodeStatus::kPayloadTooLarge;
  }
  if (datagram.size() != kPacketHeaderSize + static_cast<std::size_t>(hdr.payload_len)) {
    return DecodeStatus::kLengthMismatch;
  }

  // Compute CRC32 over bytes [0..31], four zero bytes for [32..35], then payload [36..end).
  static constexpr uint8_t kZero4[4] = {0, 0, 0, 0};
  uint32_t computed = crc32_update(0U, datagram.subspan(0, 32));
  computed = crc32_update(computed, std::span<const uint8_t>(kZero4, 4));
  if (hdr.payload_len > 0) {
    computed = crc32_update(computed, datagram.subspan(kPacketHeaderSize, hdr.payload_len));
  }
  if (computed != hdr.checksum) {
    return DecodeStatus::kBadChecksum;
  }

  if ((hdr.flags & kFlagHeartbeat) != 0U && (hdr.msg_count != 0U || hdr.payload_len != 0U)) {
    return DecodeStatus::kInvalidField;
  }

  out_header = hdr;
  return DecodeStatus::kOk;
}

DecodeStatus decode_packet(std::span<const uint8_t> datagram, DecodedPacket& out_packet) {
  PacketHeader hdr{};
  const DecodeStatus hdr_status = decode_packet_header(datagram, hdr);
  if (hdr_status != DecodeStatus::kOk) {
    return hdr_status;
  }

  out_packet.header = hdr;
  out_packet.events.clear();
  out_packet.events.reserve(hdr.msg_count);

  Cursor cur(datagram.subspan(kPacketHeaderSize, hdr.payload_len));
  for (uint16_t i = 0; i < hdr.msg_count; ++i) {
    uint16_t record_len = 0;
    if (!cur.read_u16(record_len)) {
      return DecodeStatus::kTruncated;
    }
    if (record_len < 1 || cur.remaining() < record_len) {
      return DecodeStatus::kTruncated;
    }
    std::span<const uint8_t> rec_bytes;
    if (!cur.read_bytes(record_len, rec_bytes)) {
      return DecodeStatus::kTruncated;
    }

    Cursor rc(rec_bytes);
    uint8_t raw_type = 0;
    if (!rc.read_u8(raw_type)) {
      return DecodeStatus::kTruncated;
    }

    MarketEvent ev{};
    ev.type = static_cast<EventType>(raw_type);
    switch (ev.type) {
      case EventType::kAddOrder: {
        if (record_len != 26) return DecodeStatus::kLengthMismatch;
        uint8_t raw_side = 0;
        if (!rc.read_u32(ev.instrument_id) || !rc.read_u64(ev.order_id) || !rc.read_u8(raw_side) ||
            !rc.read_i64(ev.price_ticks) || !rc.read_u32(ev.qty)) {
          return DecodeStatus::kTruncated;
        }
        if (raw_side != static_cast<uint8_t>(Side::kBid) &&
            raw_side != static_cast<uint8_t>(Side::kAsk)) {
          return DecodeStatus::kInvalidField;
        }
        ev.side = static_cast<Side>(raw_side);
        break;
      }
      case EventType::kModifyOrder: {
        if (record_len != 25) return DecodeStatus::kLengthMismatch;
        if (!rc.read_u32(ev.instrument_id) || !rc.read_u64(ev.order_id) ||
            !rc.read_i64(ev.price_ticks) || !rc.read_u32(ev.qty)) {
          return DecodeStatus::kTruncated;
        }
        break;
      }
      case EventType::kCancelOrder: {
        if (record_len != 17) return DecodeStatus::kLengthMismatch;
        if (!rc.read_u32(ev.instrument_id) || !rc.read_u64(ev.order_id) || !rc.read_u32(ev.qty)) {
          return DecodeStatus::kTruncated;
        }
        break;
      }
      case EventType::kTrade: {
        if (record_len != 25) return DecodeStatus::kLengthMismatch;
        if (!rc.read_u32(ev.instrument_id) || !rc.read_u64(ev.order_id) ||
            !rc.read_i64(ev.price_ticks) || !rc.read_u32(ev.qty)) {
          return DecodeStatus::kTruncated;
        }
        break;
      }
      case EventType::kClearBook: {
        if (record_len != 5) return DecodeStatus::kLengthMismatch;
        if (!rc.read_u32(ev.instrument_id)) {
          return DecodeStatus::kTruncated;
        }
        break;
      }
      default:
        return DecodeStatus::kBadEventType;
    }

    out_packet.events.push_back(ev);
  }

  if (cur.remaining() != 0) {
    return DecodeStatus::kBadEventCount;
  }
  return DecodeStatus::kOk;
}

bool encode_retransmit_request(const RetransmitRequest& req, std::vector<uint8_t>& out_frame) {
  std::vector<uint8_t> payload;
  payload.reserve(18);
  append_u16_be(payload, req.channel_id);
  append_u64_be(payload, req.from_seq);
  append_u64_be(payload, req.to_seq);
  return write_recovery_frame(RecoveryMsgType::kRetransmitReq, payload, out_frame);
}

bool encode_retransmit_response(const RetransmitResponse& resp, std::vector<uint8_t>& out_frame) {
  if (resp.raw_packets.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  std::vector<uint8_t> payload;
  payload.reserve(32 + resp.raw_packets.size() * 64);
  append_u16_be(payload, resp.channel_id);
  append_u16_be(payload, static_cast<uint16_t>(resp.status));
  append_u64_be(payload, resp.oldest_available_seq);
  append_u64_be(payload, resp.latest_seq);
  append_u64_be(payload, resp.latest_snapshot_seq);
  append_u32_be(payload, static_cast<uint32_t>(resp.raw_packets.size()));

  for (const auto& pkt : resp.raw_packets) {
    if (pkt.size() > kMaxUdpDatagramSize) {
      return false;
    }
    append_u32_be(payload, static_cast<uint32_t>(pkt.size()));
    payload.insert(payload.end(), pkt.begin(), pkt.end());
  }
  return write_recovery_frame(RecoveryMsgType::kRetransmitResp, payload, out_frame);
}

bool encode_snapshot_request(const SnapshotRequest& req, std::vector<uint8_t>& out_frame) {
  std::vector<uint8_t> payload;
  payload.reserve(2);
  append_u16_be(payload, req.channel_id);
  return write_recovery_frame(RecoveryMsgType::kSnapshotReq, payload, out_frame);
}

bool encode_snapshot_response(const SnapshotResponse& resp, std::vector<uint8_t>& out_frame) {
  if (resp.snapshot.orders.size() > std::numeric_limits<uint32_t>::max() ||
      resp.snapshot.instrument_stats.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  std::vector<uint8_t> payload;
  payload.reserve(32 + resp.snapshot.orders.size() * 25 +
                  resp.snapshot.instrument_stats.size() * 32);
  append_u16_be(payload, static_cast<uint16_t>(resp.status));
  append_u16_be(payload, resp.snapshot.channel_id);
  append_u64_be(payload, resp.snapshot.as_of_seq);
  append_u64_be(payload, resp.snapshot.checkpoint_id);
  append_u32_be(payload, static_cast<uint32_t>(resp.snapshot.orders.size()));
  append_u32_be(payload, static_cast<uint32_t>(resp.snapshot.instrument_stats.size()));

  for (const auto& o : resp.snapshot.orders) {
    if (o.side != Side::kBid && o.side != Side::kAsk) {
      return false;
    }
    append_u32_be(payload, o.instrument_id);
    append_u64_be(payload, o.order_id);
    append_u8(payload, static_cast<uint8_t>(o.side));
    append_i64_be(payload, o.price_ticks);
    append_u32_be(payload, o.qty);
  }
  for (const auto& st : resp.snapshot.instrument_stats) {
    append_u32_be(payload, st.instrument_id);
    append_i64_be(payload, st.last_trade_price_ticks);
    append_u32_be(payload, st.last_trade_qty);
    append_u64_be(payload, st.trade_count);
    append_u64_be(payload, st.total_volume);
  }
  const uint32_t sum = crc32(payload);
  append_u32_be(payload, sum);

  return write_recovery_frame(RecoveryMsgType::kSnapshotResp, payload, out_frame);
}

DecodeStatus decode_recovery_frame_header(std::span<const uint8_t> bytes,
                                          RecoveryFrameHeader& out_header) {
  if (bytes.size() < kRecoveryFrameHeaderSize) {
    return DecodeStatus::kTruncated;
  }
  RecoveryFrameHeader hdr{};
  hdr.magic = read_u32_be(bytes.data() + 0);
  if (hdr.magic != kRecoveryMagic) {
    return DecodeStatus::kBadMagic;
  }
  hdr.version = read_u16_be(bytes.data() + 4);
  if (hdr.version != kProtocolVersion) {
    return DecodeStatus::kUnsupportedVersion;
  }
  const uint16_t raw_type = read_u16_be(bytes.data() + 6);
  if (raw_type < static_cast<uint16_t>(RecoveryMsgType::kRetransmitReq) ||
      raw_type > static_cast<uint16_t>(RecoveryMsgType::kSnapshotResp)) {
    return DecodeStatus::kBadEventType;
  }
  hdr.type = static_cast<RecoveryMsgType>(raw_type);
  hdr.payload_len = read_u32_be(bytes.data() + 8);
  if (hdr.payload_len > kMaxRecoveryFramePayload) {
    return DecodeStatus::kPayloadTooLarge;
  }
  out_header = hdr;
  return DecodeStatus::kOk;
}

DecodeStatus decode_retransmit_request(std::span<const uint8_t> payload, RetransmitRequest& out) {
  if (payload.size() != 18) {
    return DecodeStatus::kLengthMismatch;
  }
  Cursor cur(payload);
  if (!cur.read_u16(out.channel_id) || !cur.read_u64(out.from_seq) || !cur.read_u64(out.to_seq)) {
    return DecodeStatus::kTruncated;
  }
  return DecodeStatus::kOk;
}

DecodeStatus decode_retransmit_response(std::span<const uint8_t> payload, RetransmitResponse& out) {
  Cursor cur(payload);
  uint16_t raw_status = 0;
  uint32_t pkt_count = 0;
  if (!cur.read_u16(out.channel_id) || !cur.read_u16(raw_status) ||
      !cur.read_u64(out.oldest_available_seq) || !cur.read_u64(out.latest_seq) ||
      !cur.read_u64(out.latest_snapshot_seq) || !cur.read_u32(pkt_count)) {
    return DecodeStatus::kTruncated;
  }
  if (raw_status > static_cast<uint16_t>(RecoveryStatus::kInvalidRequest)) {
    return DecodeStatus::kInvalidField;
  }
  // Sanity cap: each packet needs at least 4 bytes length prefix + 36 bytes header.
  if (static_cast<uint64_t>(pkt_count) * 40ULL > cur.remaining()) {
    return DecodeStatus::kTruncated;
  }
  out.status = static_cast<RecoveryStatus>(raw_status);
  out.raw_packets.clear();
  out.raw_packets.reserve(pkt_count);
  for (uint32_t i = 0; i < pkt_count; ++i) {
    uint32_t pkt_len = 0;
    if (!cur.read_u32(pkt_len)) {
      return DecodeStatus::kTruncated;
    }
    if (pkt_len > kMaxUdpDatagramSize) {
      return DecodeStatus::kPayloadTooLarge;
    }
    std::span<const uint8_t> pkt_bytes;
    if (!cur.read_bytes(pkt_len, pkt_bytes)) {
      return DecodeStatus::kTruncated;
    }
    out.raw_packets.emplace_back(pkt_bytes.begin(), pkt_bytes.end());
  }
  if (cur.remaining() != 0) {
    return DecodeStatus::kLengthMismatch;
  }
  return DecodeStatus::kOk;
}

DecodeStatus decode_snapshot_request(std::span<const uint8_t> payload, SnapshotRequest& out) {
  if (payload.size() != 2) {
    return DecodeStatus::kLengthMismatch;
  }
  Cursor cur(payload);
  if (!cur.read_u16(out.channel_id)) {
    return DecodeStatus::kTruncated;
  }
  return DecodeStatus::kOk;
}

DecodeStatus decode_snapshot_response(std::span<const uint8_t> payload, SnapshotResponse& out) {
  if (payload.size() < 32) {
    return DecodeStatus::kTruncated;
  }
  // Last 4 bytes are CRC32 over preceding payload bytes.
  const std::span<const uint8_t> body = payload.subspan(0, payload.size() - 4);
  const uint32_t expected_crc = read_u32_be(payload.data() + payload.size() - 4);
  if (crc32(body) != expected_crc) {
    return DecodeStatus::kBadChecksum;
  }

  Cursor cur(body);
  uint16_t raw_status = 0;
  uint32_t order_count = 0;
  uint32_t stat_count = 0;
  if (!cur.read_u16(raw_status) || !cur.read_u16(out.snapshot.channel_id) ||
      !cur.read_u64(out.snapshot.as_of_seq) || !cur.read_u64(out.snapshot.checkpoint_id) ||
      !cur.read_u32(order_count) || !cur.read_u32(stat_count)) {
    return DecodeStatus::kTruncated;
  }
  if (raw_status > static_cast<uint16_t>(RecoveryStatus::kInvalidRequest)) {
    return DecodeStatus::kInvalidField;
  }
  const uint64_t expected_bytes =
      static_cast<uint64_t>(order_count) * 25ULL + static_cast<uint64_t>(stat_count) * 32ULL;
  if (cur.remaining() != expected_bytes) {
    return DecodeStatus::kLengthMismatch;
  }

  out.status = static_cast<RecoveryStatus>(raw_status);
  out.snapshot.orders.clear();
  out.snapshot.orders.reserve(order_count);
  for (uint32_t i = 0; i < order_count; ++i) {
    SnapshotOrder o{};
    uint8_t raw_side = 0;
    if (!cur.read_u32(o.instrument_id) || !cur.read_u64(o.order_id) || !cur.read_u8(raw_side) ||
        !cur.read_i64(o.price_ticks) || !cur.read_u32(o.qty)) {
      return DecodeStatus::kTruncated;
    }
    if (raw_side != static_cast<uint8_t>(Side::kBid) &&
        raw_side != static_cast<uint8_t>(Side::kAsk)) {
      return DecodeStatus::kInvalidField;
    }
    o.side = static_cast<Side>(raw_side);
    out.snapshot.orders.push_back(o);
  }

  out.snapshot.instrument_stats.clear();
  out.snapshot.instrument_stats.reserve(stat_count);
  for (uint32_t i = 0; i < stat_count; ++i) {
    SnapshotInstrumentStats st{};
    if (!cur.read_u32(st.instrument_id) || !cur.read_i64(st.last_trade_price_ticks) ||
        !cur.read_u32(st.last_trade_qty) || !cur.read_u64(st.trade_count) ||
        !cur.read_u64(st.total_volume)) {
      return DecodeStatus::kTruncated;
    }
    out.snapshot.instrument_stats.push_back(st);
  }
  return DecodeStatus::kOk;
}

uint64_t steady_now_ns() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace mdf
