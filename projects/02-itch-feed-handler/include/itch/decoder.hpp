// Bounds-checked ITCH 5.0 message body decoder.
//
// Input is one message payload (type byte first) with transport framing
// already removed; see framing.hpp. The decoder:
//   * checks the payload length against the specification length for the
//     type *before* reading any field,
//   * reads fields sequentially through a Cursor using byte-wise big-endian
//     loads (no struct overlay, no unaligned or type-punned access),
//   * proves at compile time (static_assert below) that each reader consumes
//     exactly kLength bytes, so a reader can never run past a buffer that
//     passed the length check.
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

enum class DecodeStatus : std::uint8_t {
  Ok,
  Empty,         // zero-length payload
  UnknownType,   // type byte not in the supported-message matrix
  Truncated,     // payload shorter than the specification length
  Oversized,     // payload longer than the specification length
  InvalidField,  // an enumerated field held a value the spec does not allow
};

[[nodiscard]] constexpr const char* to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Ok:
      return "ok";
    case DecodeStatus::Empty:
      return "empty";
    case DecodeStatus::UnknownType:
      return "unknown_type";
    case DecodeStatus::Truncated:
      return "truncated";
    case DecodeStatus::Oversized:
      return "oversized";
    case DecodeStatus::InvalidField:
      return "invalid_field";
  }
  return "?";
}

namespace detail {

class Cursor {
 public:
  constexpr Cursor(const std::uint8_t* p, std::size_t n) noexcept : p_(p), end_(p + n) {}

  constexpr std::uint8_t u8() noexcept {
    need(1);
    return *p_++;
  }
  constexpr char ch() noexcept { return static_cast<char>(u8()); }
  constexpr std::uint16_t u16() noexcept {
    need(2);
    const auto v = load_be16(p_);
    p_ += 2;
    return v;
  }
  constexpr std::uint32_t u32() noexcept {
    need(4);
    const auto v = load_be32(p_);
    p_ += 4;
    return v;
  }
  constexpr std::uint64_t u48() noexcept {
    need(6);
    const auto v = load_be48(p_);
    p_ += 6;
    return v;
  }
  constexpr std::uint64_t u64() noexcept {
    need(8);
    const auto v = load_be64(p_);
    p_ += 8;
    return v;
  }
  template <std::size_t N>
  constexpr void chars(std::array<char, N>& out) noexcept {
    need(N);
    for (std::size_t i = 0; i < N; ++i) out[i] = static_cast<char>(p_[i]);
    p_ += N;
  }
  [[nodiscard]] constexpr std::size_t remaining() const noexcept {
    return static_cast<std::size_t>(end_ - p_);
  }

 private:
  constexpr void need([[maybe_unused]] std::size_t k) const noexcept { assert(remaining() >= k); }

  const std::uint8_t* p_;
  const std::uint8_t* end_;
};

constexpr void read_header(Cursor& c, Header& h) noexcept {
  (void)c.u8();  // message type
  h.stock_locate = c.u16();
  h.tracking_number = c.u16();
  h.timestamp = c.u48();
}

constexpr void read(Cursor& c, SystemEvent& m) noexcept {
  read_header(c, m.hdr);
  m.event_code = c.ch();
}

constexpr void read(Cursor& c, StockDirectory& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.market_category = c.ch();
  m.financial_status = c.ch();
  m.round_lot_size = c.u32();
  m.round_lots_only = c.ch();
  m.issue_classification = c.ch();
  c.chars(m.issue_subtype);
  m.authenticity = c.ch();
  m.short_sale_threshold = c.ch();
  m.ipo_flag = c.ch();
  m.luld_tier = c.ch();
  m.etp_flag = c.ch();
  m.etp_leverage = c.u32();
  m.inverse_indicator = c.ch();
}

constexpr void read(Cursor& c, StockTradingAction& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.trading_state = c.ch();
  m.reserved = c.ch();
  c.chars(m.reason);
}

constexpr void read(Cursor& c, RegShoRestriction& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.action = c.ch();
}

constexpr void read(Cursor& c, MarketParticipantPosition& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.mpid);
  c.chars(m.stock.raw);
  m.primary_market_maker = c.ch();
  m.market_maker_mode = c.ch();
  m.participant_state = c.ch();
}

constexpr void read(Cursor& c, MwcbDeclineLevel& m) noexcept {
  read_header(c, m.hdr);
  m.level1 = c.u64();
  m.level2 = c.u64();
  m.level3 = c.u64();
}

constexpr void read(Cursor& c, MwcbStatus& m) noexcept {
  read_header(c, m.hdr);
  m.breached_level = c.ch();
}

constexpr void read(Cursor& c, IpoQuotingPeriod& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.release_time = c.u32();
  m.release_qualifier = c.ch();
  m.ipo_price = c.u32();
}

constexpr void read(Cursor& c, LuldAuctionCollar& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.reference_price = c.u32();
  m.upper_price = c.u32();
  m.lower_price = c.u32();
  m.extension = c.u32();
}

constexpr void read(Cursor& c, OperationalHalt& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.market_code = c.ch();
  m.action = c.ch();
}

constexpr void read(Cursor& c, AddOrder& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.side = static_cast<Side>(c.ch());
  m.shares = c.u32();
  c.chars(m.stock.raw);
  m.price = c.u32();
}

constexpr void read(Cursor& c, AddOrderMpid& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.side = static_cast<Side>(c.ch());
  m.shares = c.u32();
  c.chars(m.stock.raw);
  m.price = c.u32();
  c.chars(m.attribution);
}

constexpr void read(Cursor& c, OrderExecuted& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.executed_shares = c.u32();
  m.match_number = c.u64();
}

constexpr void read(Cursor& c, OrderExecutedWithPrice& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.executed_shares = c.u32();
  m.match_number = c.u64();
  m.printable = c.ch();
  m.execution_price = c.u32();
}

constexpr void read(Cursor& c, OrderCancel& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.cancelled_shares = c.u32();
}

constexpr void read(Cursor& c, OrderDelete& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
}

constexpr void read(Cursor& c, OrderReplace& m) noexcept {
  read_header(c, m.hdr);
  m.original_order_ref = c.u64();
  m.new_order_ref = c.u64();
  m.shares = c.u32();
  m.price = c.u32();
}

constexpr void read(Cursor& c, Trade& m) noexcept {
  read_header(c, m.hdr);
  m.order_ref = c.u64();
  m.side = static_cast<Side>(c.ch());
  m.shares = c.u32();
  c.chars(m.stock.raw);
  m.price = c.u32();
  m.match_number = c.u64();
}

constexpr void read(Cursor& c, CrossTrade& m) noexcept {
  read_header(c, m.hdr);
  m.shares = c.u64();
  c.chars(m.stock.raw);
  m.cross_price = c.u32();
  m.match_number = c.u64();
  m.cross_type = c.ch();
}

constexpr void read(Cursor& c, BrokenTrade& m) noexcept {
  read_header(c, m.hdr);
  m.match_number = c.u64();
}

constexpr void read(Cursor& c, Noii& m) noexcept {
  read_header(c, m.hdr);
  m.paired_shares = c.u64();
  m.imbalance_shares = c.u64();
  m.imbalance_direction = c.ch();
  c.chars(m.stock.raw);
  m.far_price = c.u32();
  m.near_price = c.u32();
  m.current_reference_price = c.u32();
  m.cross_type = c.ch();
  m.price_variation_indicator = c.ch();
}

constexpr void read(Cursor& c, Rpii& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.interest_flag = c.ch();
}

constexpr void read(Cursor& c, DlcrPriceDiscovery& m) noexcept {
  read_header(c, m.hdr);
  c.chars(m.stock.raw);
  m.open_eligibility_status = c.ch();
  m.min_allowable_price = c.u32();
  m.max_allowable_price = c.u32();
  m.near_execution_price = c.u32();
  m.near_execution_time = c.u64();
  m.lower_price_range_collar = c.u32();
  m.upper_price_range_collar = c.u32();
}

// Number of bytes read(Cursor&, T&) consumes, evaluated at compile time.
template <class T>
constexpr std::size_t wire_size() noexcept {
  std::array<std::uint8_t, 64> buf{};
  Cursor c{buf.data(), buf.size()};
  T m{};
  read(c, m);
  return buf.size() - c.remaining();
}

#define ITCH_SIZE_CHECK(T)                                                                  \
  static_assert(wire_size<T>() == T::kLength, #T " reader does not match its wire length"); \
  static_assert(T::kLength <= kMaxMessageLength);
ITCH_FOR_EACH_MESSAGE(ITCH_SIZE_CHECK)
#undef ITCH_SIZE_CHECK

constexpr bool valid_side(Side s) noexcept { return s == Side::Buy || s == Side::Sell; }
constexpr bool valid(const AddOrder& m) noexcept { return valid_side(m.side); }
constexpr bool valid(const AddOrderMpid& m) noexcept { return valid_side(m.side); }
constexpr bool valid(const Trade& m) noexcept { return valid_side(m.side); }
template <class T>
constexpr bool valid(const T&) noexcept {
  return true;
}

}  // namespace detail

// Decodes one message payload and, on success, invokes `handler(const T&)`
// with the typed message. The handler must be callable with every message
// type (a generic `template <class T> void operator()(const T&)` fallback is
// the usual way). On any non-Ok status the handler is not invoked.
template <class Handler>
[[nodiscard]] inline DecodeStatus decode(std::span<const std::uint8_t> payload, Handler&& handler) {
  if (payload.empty()) return DecodeStatus::Empty;
  const std::uint8_t* p = payload.data();
  const std::size_t n = payload.size();
  switch (static_cast<char>(p[0])) {
#define ITCH_DECODE_CASE(T)                                                      \
  case T::kType: {                                                               \
    if (n != T::kLength) {                                                       \
      return n < T::kLength ? DecodeStatus::Truncated : DecodeStatus::Oversized; \
    }                                                                            \
    T m;                                                                         \
    detail::Cursor c{p, n};                                                      \
    detail::read(c, m);                                                          \
    if (!detail::valid(m)) return DecodeStatus::InvalidField;                    \
    handler(std::as_const(m));                                                   \
    return DecodeStatus::Ok;                                                     \
  }
    ITCH_FOR_EACH_MESSAGE(ITCH_DECODE_CASE)
#undef ITCH_DECODE_CASE
    default:
      return DecodeStatus::UnknownType;
  }
}

// Owning sum type over all supported messages, for callers that prefer a
// value to a visitor (tests, tooling). The hot path uses decode() directly.
#define ITCH_VARIANT_MEMBER(T) , T
using Message = std::variant<std::monostate ITCH_FOR_EACH_MESSAGE(ITCH_VARIANT_MEMBER)>;
#undef ITCH_VARIANT_MEMBER

[[nodiscard]] inline DecodeStatus decode_message(std::span<const std::uint8_t> payload,
                                                 Message& out) {
  return decode(payload, [&out](const auto& m) { out = m; });
}

}  // namespace itch
