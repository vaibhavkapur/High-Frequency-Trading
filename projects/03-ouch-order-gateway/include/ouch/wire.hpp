// OUCH 4.2-style binary message codec (see docs/protocol.md for the supported
// subset and deliberate simplifications). All integers are big-endian.
//
// Naming follows the specification's point of view: "inbound" messages travel
// client -> exchange, "outbound" messages travel exchange -> client.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "ouch/types.hpp"

namespace ouch::wire {

// ---------------------------------------------------------------- inbound

struct EnterOrder {
  static constexpr char kType = 'O';
  static constexpr std::size_t kSize = 49;
  Token token;
  char side = 'B';  // B buy, S sell, T sell short, E sell short exempt
  std::uint32_t shares = 0;
  Symbol stock;
  std::uint32_t price = 0;
  std::uint32_t tif = kTifMarketHours;
  Firm firm;
  char display = 'Y';
  char capacity = 'A';
  char iso = 'N';  // intermarket sweep eligibility
  std::uint32_t min_qty = 0;
  char cross_type = 'N';
  char customer_type = ' ';
  friend bool operator==(const EnterOrder&, const EnterOrder&) = default;
};

struct ReplaceOrder {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 47;
  Token existing_token;
  Token replacement_token;
  std::uint32_t shares = 0;  // new total size, inclusive of shares already executed
  std::uint32_t price = 0;
  std::uint32_t tif = kTifMarketHours;
  char display = 'Y';
  char iso = 'N';
  std::uint32_t min_qty = 0;
  friend bool operator==(const ReplaceOrder&, const ReplaceOrder&) = default;
};

struct CancelOrder {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 19;
  Token token;
  std::uint32_t shares = 0;  // intended remaining open quantity; 0 cancels everything
  friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

using InboundMessage = std::variant<EnterOrder, ReplaceOrder, CancelOrder>;

// --------------------------------------------------------------- outbound

struct SystemEvent {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 10;
  std::uint64_t timestamp = 0;  // nanoseconds since midnight
  char event_code = 'S';        // S start of day, E end of day
  friend bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

struct Accepted {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 66;
  std::uint64_t timestamp = 0;
  Token token;
  char side = 'B';
  std::uint32_t shares = 0;
  Symbol stock;
  std::uint32_t price = 0;
  std::uint32_t tif = 0;
  Firm firm;
  char display = 'Y';
  std::uint64_t order_ref = 0;
  char capacity = 'A';
  char iso = 'N';
  std::uint32_t min_qty = 0;
  char cross_type = 'N';
  char order_state = 'L';  // L live, D dead
  char bbo_weight = ' ';
  friend bool operator==(const Accepted&, const Accepted&) = default;
};

struct Replaced {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 80;
  std::uint64_t timestamp = 0;
  Token token;  // replacement order token
  char side = 'B';
  std::uint32_t shares = 0;  // shares outstanding on the replacement order
  Symbol stock;
  std::uint32_t price = 0;
  std::uint32_t tif = 0;
  Firm firm;
  char display = 'Y';
  std::uint64_t order_ref = 0;
  char capacity = 'A';
  char iso = 'N';
  std::uint32_t min_qty = 0;
  char cross_type = 'N';
  char order_state = 'L';
  Token previous_token;
  char bbo_weight = ' ';
  friend bool operator==(const Replaced&, const Replaced&) = default;
};

struct Canceled {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 28;
  std::uint64_t timestamp = 0;
  Token token;
  std::uint32_t decrement_shares = 0;
  char reason = 'U';  // U user, I IOC, T timeout, S supervisory, Z system ...
  friend bool operator==(const Canceled&, const Canceled&) = default;
};

struct Executed {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 40;
  std::uint64_t timestamp = 0;
  Token token;
  std::uint32_t shares = 0;
  std::uint32_t price = 0;
  char liquidity = 'A';  // A added, R removed
  std::uint64_t match_number = 0;
  friend bool operator==(const Executed&, const Executed&) = default;
};

struct Rejected {
  static constexpr char kType = 'J';
  static constexpr std::size_t kSize = 24;
  std::uint64_t timestamp = 0;
  Token token;
  char reason = 'O';
  friend bool operator==(const Rejected&, const Rejected&) = default;
};

using OutboundMessage = std::variant<SystemEvent, Accepted, Replaced, Canceled, Executed, Rejected>;

// Largest encoded message in either direction.
inline constexpr std::size_t kMaxMessageSize = Replaced::kSize;

enum class DecodeError : std::uint8_t {
  None = 0,
  Empty,        // zero-length payload
  UnknownType,  // message type outside the supported subset
  BadLength,    // payload length does not match the fixed message size
};

const char* to_string(DecodeError e);

// Encoders write exactly kSize bytes and return that count, or 0 if `out` is
// too small. Decoders are structural: they check type and exact length, but do
// not judge enumerated field values (see validate()).
std::size_t encode(const EnterOrder& m, std::span<std::uint8_t> out);
std::size_t encode(const ReplaceOrder& m, std::span<std::uint8_t> out);
std::size_t encode(const CancelOrder& m, std::span<std::uint8_t> out);
std::size_t encode(const SystemEvent& m, std::span<std::uint8_t> out);
std::size_t encode(const Accepted& m, std::span<std::uint8_t> out);
std::size_t encode(const Replaced& m, std::span<std::uint8_t> out);
std::size_t encode(const Canceled& m, std::span<std::uint8_t> out);
std::size_t encode(const Executed& m, std::span<std::uint8_t> out);
std::size_t encode(const Rejected& m, std::span<std::uint8_t> out);
std::size_t encode(const InboundMessage& m, std::span<std::uint8_t> out);
std::size_t encode(const OutboundMessage& m, std::span<std::uint8_t> out);

DecodeError decode_inbound(std::span<const std::uint8_t> in, InboundMessage& out);
DecodeError decode_outbound(std::span<const std::uint8_t> in, OutboundMessage& out);

// Semantic validation used by the exchange simulator. Returns 0 when the
// message is acceptable, otherwise an OUCH reject reason code.
char validate(const EnterOrder& m);
char validate(const ReplaceOrder& m);

}  // namespace ouch::wire
