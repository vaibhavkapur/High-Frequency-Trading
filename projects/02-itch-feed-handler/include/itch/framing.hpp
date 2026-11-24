// Transport framing, kept separate from message-body decoding.
//
// Two framings are supported, both as pure buffer parsers (no sockets):
//   * LengthPrefixedReader - Nasdaq historical "BinaryFILE" layout: each
//     message is preceded by a 2-byte big-endian length.
//   * MoldUDP64            - downstream packet layout: 20-byte header then
//     `count` message blocks, each a 2-byte big-endian length plus payload.
//
// Neither framer looks inside a payload; that is decoder.hpp's job.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "itch/endian.hpp"

namespace itch {

// ---- Length-prefixed file framing ------------------------------------------

enum class FrameStatus : std::uint8_t {
  Ok,
  End,               // clean end of input
  ZeroLength,        // a 0-length frame; consumed and skippable
  TruncatedLength,   // 1 stray byte where a length prefix was expected
  TruncatedPayload,  // length prefix promises more bytes than remain
};

class LengthPrefixedReader {
 public:
  explicit LengthPrefixedReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  // On Ok, `payload` is the next message body. ZeroLength consumes the prefix
  // and may be followed by further calls. The Truncated* statuses are
  // terminal: a length-prefixed stream has no resynchronisation point, so the
  // reader does not advance and every later call returns the same status.
  [[nodiscard]] FrameStatus next(std::span<const std::uint8_t>& payload) noexcept {
    const std::size_t left = data_.size() - pos_;
    if (left == 0) return FrameStatus::End;
    if (left < 2) return FrameStatus::TruncatedLength;
    const std::size_t len = load_be16(data_.data() + pos_);
    if (len == 0) {
      pos_ += 2;
      return FrameStatus::ZeroLength;
    }
    if (left - 2 < len) return FrameStatus::TruncatedPayload;
    payload = data_.subspan(pos_ + 2, len);
    pos_ += 2 + len;
    return FrameStatus::Ok;
  }

  [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
};

// ---- MoldUDP64 --------------------------------------------------------------

inline constexpr std::size_t kMoldHeaderLength = 20;
inline constexpr std::uint16_t kMoldHeartbeat = 0;
inline constexpr std::uint16_t kMoldEndOfSession = 0xFFFF;

struct MoldHeader {
  std::array<char, 10> session{};
  std::uint64_t sequence{};  // sequence number of the first message in the packet
  std::uint16_t count{};
};

enum class MoldStatus : std::uint8_t {
  Ok,
  Heartbeat,
  EndOfSession,
  TruncatedHeader,
  TruncatedBlock,  // a message block ran past the end of the packet
};

// Parses one downstream packet, calling `on_message(sequence, payload)` for
// each complete message block. On TruncatedBlock the blocks before the bad
// one have already been delivered.
template <class F>
[[nodiscard]] inline MoldStatus parse_moldudp64(std::span<const std::uint8_t> packet,
                                                MoldHeader& hdr, F&& on_message) {
  if (packet.size() < kMoldHeaderLength) return MoldStatus::TruncatedHeader;
  const std::uint8_t* p = packet.data();
  for (std::size_t i = 0; i < hdr.session.size(); ++i) hdr.session[i] = static_cast<char>(p[i]);
  hdr.sequence = load_be64(p + 10);
  hdr.count = load_be16(p + 18);
  if (hdr.count == kMoldHeartbeat) return MoldStatus::Heartbeat;
  if (hdr.count == kMoldEndOfSession) return MoldStatus::EndOfSession;

  std::size_t pos = kMoldHeaderLength;
  for (std::uint16_t i = 0; i < hdr.count; ++i) {
    if (packet.size() - pos < 2) return MoldStatus::TruncatedBlock;
    const std::size_t len = load_be16(p + pos);
    pos += 2;
    if (packet.size() - pos < len) return MoldStatus::TruncatedBlock;
    on_message(hdr.sequence + i, packet.subspan(pos, len));
    pos += len;
  }
  return MoldStatus::Ok;
}

// Tracks the per-session sequence so that each message is delivered at most
// once and losses are counted. It only detects gaps: issuing retransmission
// requests needs a live rewinder connection and is out of scope.
class MoldSequenceTracker {
 public:
  explicit MoldSequenceTracker(std::uint64_t first_expected = 1) noexcept
      : expected_(first_expected) {}

  template <class F>
  [[nodiscard]] MoldStatus on_packet(std::span<const std::uint8_t> packet, F&& on_message) {
    MoldHeader hdr;
    const MoldStatus st =
        parse_moldudp64(packet, hdr, [&](std::uint64_t seq, std::span<const std::uint8_t> msg) {
          if (seq < expected_) {
            ++duplicates_;
            return;
          }
          note_gap(seq);
          on_message(seq, msg);
          expected_ = seq + 1;
        });
    // Heartbeats and end-of-session carry the next expected sequence number,
    // so they reveal a loss even when no further data packet arrives.
    if (st == MoldStatus::Heartbeat || st == MoldStatus::EndOfSession) {
      note_gap(hdr.sequence);
      if (hdr.sequence > expected_) expected_ = hdr.sequence;
    }
    return st;
  }

  [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
  [[nodiscard]] std::uint64_t missed_messages() const noexcept { return missed_; }
  [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }

 private:
  void note_gap(std::uint64_t seq) noexcept {
    if (seq > expected_) {
      ++gaps_;
      missed_ += seq - expected_;
    }
  }

  std::uint64_t expected_;
  std::uint64_t gaps_ = 0;
  std::uint64_t missed_ = 0;
  std::uint64_t duplicates_ = 0;
};

}  // namespace itch
