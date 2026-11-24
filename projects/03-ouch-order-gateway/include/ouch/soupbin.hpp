// SoupBinTCP 3.00-style session framing.
//
// Every packet is: 2-byte big-endian length (counting the type byte and the
// payload), 1-byte packet type, payload. Sequenced messages carry no explicit
// sequence number; both sides count them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ouch/types.hpp"

namespace ouch::soup {

inline constexpr char kDebug = '+';
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequencedData = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';
inline constexpr char kLoginRequest = 'L';
inline constexpr char kUnsequencedData = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';

inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionNotAvailable = 'S';

using Username = Alpha<6>;
using Password = Alpha<10>;
using SessionId = Alpha<10>;

struct LoginRequest {
  static constexpr std::size_t kSize = 46;  // payload only
  Username username;
  Password password;
  SessionId session;      // blank = "whatever session is current"
  std::uint64_t seq = 0;  // next sequence number wanted; 0 = start at the current end
  friend bool operator==(const LoginRequest&, const LoginRequest&) = default;
};

struct LoginAccepted {
  static constexpr std::size_t kSize = 30;
  SessionId session;
  std::uint64_t seq = 0;  // sequence number of the next sequenced message
  friend bool operator==(const LoginAccepted&, const LoginAccepted&) = default;
};

// Appends one framed packet to `out`. Payloads larger than 65534 bytes cannot
// be framed and are refused (returns false, `out` untouched).
bool append_packet(std::vector<std::uint8_t>& out, char type,
                   std::span<const std::uint8_t> payload = {});
void append_login_request(std::vector<std::uint8_t>& out, const LoginRequest& m);
void append_login_accepted(std::vector<std::uint8_t>& out, const LoginAccepted& m);
void append_login_rejected(std::vector<std::uint8_t>& out, char reason);

bool parse_login_request(std::span<const std::uint8_t> payload, LoginRequest& out);
bool parse_login_accepted(std::span<const std::uint8_t> payload, LoginAccepted& out);

// Incremental packet reassembler for a TCP byte stream.
class FrameParser {
 public:
  enum class Status { NeedMore, Packet, Error };

  void feed(std::span<const std::uint8_t> bytes);

  // On Packet, `payload` views internal storage and stays valid until the next
  // call to feed() or next(). Error (a zero-length packet) is sticky.
  Status next(char& type, std::span<const std::uint8_t>& payload);

  void reset();
  [[nodiscard]] std::size_t buffered() const { return buf_.size() - off_; }

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t off_ = 0;
  bool error_ = false;
};

}  // namespace ouch::soup
