#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mdf {

// Wire constants for UDP Multicast Market Data Feed v1 ("MDF1").
inline constexpr uint32_t kPacketMagic = 0x4D444631U;  // "MDF1"
inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kPacketHeaderSize = 36;
inline constexpr std::size_t kMaxUdpDatagramSize = 1472;  // Standard IPv4 MTU payload budget
inline constexpr std::size_t kMaxPacketPayloadSize = kMaxUdpDatagramSize - kPacketHeaderSize;

// PacketHeader flags bitmask.
inline constexpr uint16_t kFlagNone = 0x0000U;
inline constexpr uint16_t kFlagHeartbeat = 0x0001U;
inline constexpr uint16_t kFlagRetransmit = 0x0002U;
inline constexpr uint16_t kFlagEndOfBatch = 0x0004U;

// Wire constants for TCP Snapshot & Retransmission Service v1 ("MDR1").
inline constexpr uint32_t kRecoveryMagic = 0x4D445231U;  // "MDR1"
inline constexpr std::size_t kRecoveryFrameHeaderSize = 12;
inline constexpr uint32_t kMaxRecoveryFramePayload = 32U * 1024U * 1024U;  // 32 MiB cap

enum class Side : uint8_t {
  kBid = 0,
  kAsk = 1,
};

enum class EventType : uint8_t {
  kAddOrder = 'A',
  kModifyOrder = 'M',
  kCancelOrder = 'C',
  kTrade = 'T',
  kClearBook = 'X',
};

struct MarketEvent {
  EventType type = EventType::kAddOrder;
  uint32_t instrument_id = 0;
  uint64_t order_id = 0;
  Side side = Side::kBid;
  int64_t price_ticks = 0;
  uint32_t qty = 0;

  bool operator==(const MarketEvent&) const = default;
};

struct PacketHeader {
  uint32_t magic = kPacketMagic;
  uint16_t version = kProtocolVersion;
  uint16_t flags = kFlagNone;
  uint16_t channel_id = 0;
  uint16_t msg_count = 0;
  uint32_t payload_len = 0;
  uint64_t seq_num = 0;
  uint64_t send_ts_ns = 0;
  uint32_t checksum = 0;

  bool operator==(const PacketHeader&) const = default;
};

struct DecodedPacket {
  PacketHeader header{};
  std::vector<MarketEvent> events;
};

enum class DecodeStatus : uint8_t {
  kOk = 0,
  kTruncated,
  kBadMagic,
  kUnsupportedVersion,
  kLengthMismatch,
  kBadChecksum,
  kBadEventCount,
  kBadEventType,
  kInvalidField,
  kPayloadTooLarge,
};

const char* decode_status_name(DecodeStatus status) noexcept;

// Encode a single packet (header + 0..N events) into `out` (appends/replaces).
// Computes CRC32 over header (with checksum field = 0) and serialized event payload.
// Returns false if payload exceeds kMaxPacketPayloadSize or event fields are invalid.
bool encode_packet(const PacketHeader& header_in,
                   std::span<const MarketEvent> events,
                   std::vector<uint8_t>& out);

// Convenience helper to encode a heartbeat packet carrying `last_published_seq`.
bool encode_heartbeat(uint16_t channel_id,
                      uint64_t last_published_seq,
                      uint64_t send_ts_ns,
                      std::vector<uint8_t>& out);

// Validate and decode only the 36-byte header + CRC32 over the full datagram.
DecodeStatus decode_packet_header(std::span<const uint8_t> datagram, PacketHeader& out_header);

// Validate CRC32 and decode full packet including all framed MarketEvents.
DecodeStatus decode_packet(std::span<const uint8_t> datagram, DecodedPacket& out_packet);

// --- TCP Recovery & Snapshot Protocol ("MDR1") ---

enum class RecoveryMsgType : uint16_t {
  kRetransmitReq = 1,
  kRetransmitResp = 2,
  kSnapshotReq = 3,
  kSnapshotResp = 4,
};

enum class RecoveryStatus : uint16_t {
  kOk = 0,
  kUnavailable = 1,
  kUnknownChannel = 2,
  kInvalidRequest = 3,
};

struct SnapshotOrder {
  uint32_t instrument_id = 0;
  uint64_t order_id = 0;
  Side side = Side::kBid;
  int64_t price_ticks = 0;
  uint32_t qty = 0;

  bool operator==(const SnapshotOrder&) const = default;
};

struct SnapshotInstrumentStats {
  uint32_t instrument_id = 0;
  int64_t last_trade_price_ticks = 0;
  uint32_t last_trade_qty = 0;
  uint64_t trade_count = 0;
  uint64_t total_volume = 0;

  bool operator==(const SnapshotInstrumentStats&) const = default;
};

struct SnapshotData {
  uint16_t channel_id = 0;
  uint64_t as_of_seq = 0;
  uint64_t checkpoint_id = 0;
  std::vector<SnapshotOrder> orders;
  std::vector<SnapshotInstrumentStats> instrument_stats;

  bool operator==(const SnapshotData&) const = default;
};

struct RetransmitRequest {
  uint16_t channel_id = 0;
  uint64_t from_seq = 0;
  uint64_t to_seq = 0;

  bool operator==(const RetransmitRequest&) const = default;
};

struct RetransmitResponse {
  uint16_t channel_id = 0;
  RecoveryStatus status = RecoveryStatus::kOk;
  uint64_t oldest_available_seq = 0;
  uint64_t latest_seq = 0;
  uint64_t latest_snapshot_seq = 0;
  std::vector<std::vector<uint8_t>> raw_packets;

  bool operator==(const RetransmitResponse&) const = default;
};

struct SnapshotRequest {
  uint16_t channel_id = 0;

  bool operator==(const SnapshotRequest&) const = default;
};

struct SnapshotResponse {
  RecoveryStatus status = RecoveryStatus::kOk;
  SnapshotData snapshot{};

  bool operator==(const SnapshotResponse&) const = default;
};

struct RecoveryFrameHeader {
  uint32_t magic = kRecoveryMagic;
  uint16_t version = kProtocolVersion;
  RecoveryMsgType type = RecoveryMsgType::kRetransmitReq;
  uint32_t payload_len = 0;
};

// Encode/decode TCP recovery frames.
bool encode_retransmit_request(const RetransmitRequest& req, std::vector<uint8_t>& out_frame);
bool encode_retransmit_response(const RetransmitResponse& resp, std::vector<uint8_t>& out_frame);
bool encode_snapshot_request(const SnapshotRequest& req, std::vector<uint8_t>& out_frame);
bool encode_snapshot_response(const SnapshotResponse& resp, std::vector<uint8_t>& out_frame);

DecodeStatus decode_recovery_frame_header(std::span<const uint8_t> bytes,
                                          RecoveryFrameHeader& out_header);
DecodeStatus decode_retransmit_request(std::span<const uint8_t> payload, RetransmitRequest& out);
DecodeStatus decode_retransmit_response(std::span<const uint8_t> payload, RetransmitResponse& out);
DecodeStatus decode_snapshot_request(std::span<const uint8_t> payload, SnapshotRequest& out);
DecodeStatus decode_snapshot_response(std::span<const uint8_t> payload, SnapshotResponse& out);

// Monotonic clock helper (nanoseconds).
uint64_t steady_now_ns() noexcept;

}  // namespace mdf
