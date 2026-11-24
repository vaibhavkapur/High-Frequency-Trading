#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xarb/types.hpp"

namespace xarb {

enum class FeedEventKind : uint8_t {
  WsMessage = 0,
  RestSnapshot = 1,
  Disconnect = 2,
  Reconnect = 3,
};

inline std::string_view feed_event_kind_name(FeedEventKind k) noexcept {
  switch (k) {
    case FeedEventKind::WsMessage: return "ws";
    case FeedEventKind::RestSnapshot: return "rest_snapshot";
    case FeedEventKind::Disconnect: return "disconnect";
    case FeedEventKind::Reconnect: return "reconnect";
  }
  return "unknown";
}

inline std::optional<FeedEventKind> parse_feed_event_kind(std::string_view s) noexcept {
  if (s == "ws") return FeedEventKind::WsMessage;
  if (s == "rest_snapshot") return FeedEventKind::RestSnapshot;
  if (s == "disconnect") return FeedEventKind::Disconnect;
  if (s == "reconnect") return FeedEventKind::Reconnect;
  return std::nullopt;
}

struct RecordedFeedEvent {
  TimestampNs recv_ns{0};
  VenueId venue{VenueId::Binance};
  uint32_t instrument_id{0};
  FeedEventKind kind{FeedEventKind::WsMessage};
  std::string payload;
};

// JSONL feed serialization/deserialization (human-readable & inspectable).
std::string encode_jsonl_event(const RecordedFeedEvent& ev);
std::optional<RecordedFeedEvent> decode_jsonl_event(std::string_view line, std::string* err = nullptr);

bool write_jsonl_feed(const std::string& path, const std::vector<RecordedFeedEvent>& events,
                      std::string* err = nullptr);
bool read_jsonl_feed(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err = nullptr);

// Validated binary framing format (little-endian):
//   [4B magic "XARB"] [1B version=1] [1B venue] [1B kind] [1B reserved=0]
//   [4B instrument_id] [8B recv_ns] [4B payload_len] [payload_len bytes] [4B IEEE CRC32 over header+payload]
constexpr uint32_t kBinaryFrameMagic = 0x42524158U;  // "XARB" little-endian
constexpr uint8_t kBinaryFrameVersion = 1U;
constexpr size_t kBinaryHeaderSize = 24;
constexpr size_t kBinaryFooterSize = 4;
constexpr uint32_t kMaxBinaryPayloadBytes = 2 * 1024 * 1024;

std::vector<uint8_t> encode_binary_frame(const RecordedFeedEvent& ev);

// Decodes one frame starting at data[offset]; advances offset on success.
// Returns false on malformed header, truncated buffer, payload > max, or CRC32 mismatch.
bool decode_binary_frame(const uint8_t* data, size_t len, size_t& offset, RecordedFeedEvent& out,
                         std::string* err = nullptr);

bool write_binary_feed(const std::string& path, const std::vector<RecordedFeedEvent>& events,
                       std::string* err = nullptr);
bool read_binary_feed(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err = nullptr);

// Auto-detects binary vs JSONL by magic bytes.
bool read_feed_auto(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err = nullptr);

}  // namespace xarb
