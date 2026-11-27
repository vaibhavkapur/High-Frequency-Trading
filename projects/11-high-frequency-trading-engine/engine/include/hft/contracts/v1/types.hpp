#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace hft::contracts::v1 {

static_assert(std::endian::native == std::endian::little,
              "Canonical wire serialization assumes little-endian host");

inline constexpr std::uint32_t kContractVersionMajor = 1;
inline constexpr std::uint32_t kContractVersionMinor = 0;
inline constexpr std::uint32_t kContractVersion =
    (kContractVersionMajor << 16U) | kContractVersionMinor;

// Exact fixed-point currency scale: 1 unit = $0.0001 (matches ITCH Price4 / OUCH / SOR scale).
inline constexpr std::int64_t kMoneyScale = 10'000;
inline constexpr std::int64_t kBpsScaleX100 = 1'000'000;  // 100.00% = 10,000 bps = 1,000,000 bps_x100

using InstrumentId = std::uint32_t;
using VenueId = std::uint16_t;
using OrderId = std::uint64_t;
using ParentOrderId = std::uint64_t;
using ChildOrderId = std::uint64_t;
using ExchangeOrderId = std::uint64_t;
using ExecId = std::uint64_t;
using EventSeq = std::uint64_t;

// Distinct strong type for market-feed order references (e.g., ITCH OrderRef / MDF1 order_id).
// Kept distinct from outbound ClientToken and OrderId so feed IDs can never be confused with
// outbound order-entry tokens.
struct FeedOrderRef {
  std::uint64_t value = 0;
  constexpr auto operator<=>(const FeedOrderRef&) const noexcept = default;
};

// Outbound 14-byte ASCII client token (space-padded, OUCH-compatible).
struct ClientToken {
  std::array<char, 14> bytes{};

  constexpr ClientToken() noexcept { bytes.fill(' '); }

  static constexpr ClientToken from_string(std::string_view sv) noexcept {
    ClientToken tok;
    const std::size_t n = std::min(sv.size(), tok.bytes.size());
    for (std::size_t i = 0; i < n; ++i) {
      tok.bytes[i] = sv[i];
    }
    return tok;
  }

  static ClientToken from_uint64(std::string_view prefix, std::uint64_t seq) noexcept {
    ClientToken tok;
    std::size_t pos = 0;
    const std::size_t p_len = std::min<std::size_t>(prefix.size(), 4);
    for (std::size_t i = 0; i < p_len; ++i) {
      tok.bytes[pos++] = prefix[i];
    }
    // Encode seq as fixed-width zero-padded decimal in remaining digits (up to 10 digits).
    const std::size_t digits = tok.bytes.size() - pos;
    std::uint64_t v = seq;
    for (std::size_t i = 0; i < digits; ++i) {
      const std::size_t idx = tok.bytes.size() - 1 - i;
      tok.bytes[idx] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
    return tok;
  }

  [[nodiscard]] std::string to_string() const {
    std::size_t end = bytes.size();
    while (end > 0 && bytes[end - 1] == ' ') {
      --end;
    }
    return std::string(bytes.data(), end);
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    for (char c : bytes) {
      if (c != ' ' && c != '\0') return false;
    }
    return true;
  }

  constexpr bool operator==(const ClientToken&) const noexcept = default;
  constexpr auto operator<=>(const ClientToken&) const noexcept = default;
};

struct ClientTokenHash {
  std::size_t operator()(const ClientToken& tok) const noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (char c : tok.bytes) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(h);
  }
};

// Distinct clock domains: SimTimeNs (deterministic simulation clock) vs WallTimeNs
// (monotonic host steady_clock). Neither implicitly converts to the other.
struct SimTimeNs {
  std::int64_t value = 0;

  constexpr SimTimeNs() noexcept = default;
  explicit constexpr SimTimeNs(std::int64_t ns) noexcept : value(ns) {}

  constexpr auto operator<=>(const SimTimeNs&) const noexcept = default;
  constexpr SimTimeNs operator+(std::int64_t delta_ns) const noexcept {
    return SimTimeNs{value + delta_ns};
  }
  constexpr std::int64_t operator-(SimTimeNs other) const noexcept {
    return value - other.value;
  }
};

struct WallTimeNs {
  std::uint64_t value = 0;

  constexpr WallTimeNs() noexcept = default;
  explicit constexpr WallTimeNs(std::uint64_t ns) noexcept : value(ns) {}

  constexpr auto operator<=>(const WallTimeNs&) const noexcept = default;
  constexpr std::int64_t operator-(WallTimeNs other) const noexcept {
    return static_cast<std::int64_t>(value) - static_cast<std::int64_t>(other.value);
  }
};

// Canonical fixed-point price in instrument-specific ticks.
struct Price {
  std::int64_t ticks = 0;

  constexpr Price() noexcept = default;
  explicit constexpr Price(std::int64_t t) noexcept : ticks(t) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return ticks > 0; }
  constexpr auto operator<=>(const Price&) const noexcept = default;
};

// Canonical fixed-point quantity in instrument-specific lots.
struct Quantity {
  std::uint64_t lots = 0;

  constexpr Quantity() noexcept = default;
  explicit constexpr Quantity(std::uint64_t l) noexcept : lots(l) {}

  [[nodiscard]] constexpr bool zero() const noexcept { return lots == 0; }
  constexpr auto operator<=>(const Quantity&) const noexcept = default;
};

// Canonical fixed-point monetary amount scaled by kMoneyScale ($0.0001 units).
struct Money {
  std::int64_t scaled = 0;

  constexpr Money() noexcept = default;
  explicit constexpr Money(std::int64_t s) noexcept : scaled(s) {}

  [[nodiscard]] double to_dollars() const noexcept {
    return static_cast<double>(scaled) / static_cast<double>(kMoneyScale);
  }

  constexpr auto operator<=>(const Money&) const noexcept = default;
  constexpr Money operator+(Money o) const noexcept { return Money{scaled + o.scaled}; }
  constexpr Money operator-(Money o) const noexcept { return Money{scaled - o.scaled}; }
  constexpr Money operator-() const noexcept { return Money{-scaled}; }
  constexpr Money& operator+=(Money o) noexcept {
    scaled += o.scaled;
    return *this;
  }
  constexpr Money& operator-=(Money o) noexcept {
    scaled -= o.scaled;
    return *this;
  }
};

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr const char* to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
enum class TimeInForce : std::uint8_t { GTC = 0, IOC = 1, FOK = 2, DAY = 3 };

enum class ErrorClass : std::uint8_t { None = 0, Retryable = 1, Terminal = 2 };

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  Overflow = 2,
  TickAlignmentError = 3,
  LotAlignmentError = 4,
  VersionMismatch = 5,
  DecodeError = 6,
  QueueFull = 7,
  FeedGap = 8,
  StaleMarketData = 9,
  RiskRejected = 10,
  SessionDisconnected = 11,
  VenueUnavailable = 12,
  DuplicateToken = 13,
  UnknownOrder = 14,
  InvariantViolation = 15,
};

constexpr ErrorClass classify_error(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return ErrorClass::None;
    case ErrorCode::QueueFull:
    case ErrorCode::FeedGap:
    case ErrorCode::StaleMarketData:
    case ErrorCode::SessionDisconnected:
    case ErrorCode::VenueUnavailable:
      return ErrorClass::Retryable;
    default:
      return ErrorClass::Terminal;
  }
}

enum class QueueOverflowPolicy : std::uint8_t {
  Backpressure = 0,
  Reject = 1,
  DropAndResync = 2,
};

// Documented deterministic priority order for equal event_time_ns:
// Lower numeric value executes first.
enum class PriorityClass : std::uint8_t {
  SystemControl = 0,        // Kill switch, session up/down, venue outage
  MarketDataSnapshot = 1,   // Full snapshot / resync
  MarketDataDelta = 2,      // Incremental feed update at exchange truth
  ExchangeOrderArrival = 3, // Order/cancel reaching matching engine
  ExchangeMatchAndExec = 4, // Match execution / exchange outbound report
  GatewayExecDelivery = 5,  // Execution report arriving at gateway/OMS
  StrategyObservation = 6,  // Delayed market view delivered to strategy
  StrategyOrderIntent = 7,  // Strategy submitting order/cancel intent
  Timer = 8,                // Periodic strategy/reconcile timers
};

struct EventOrderKey {
  std::int64_t event_time_ns = 0;
  PriorityClass priority = PriorityClass::MarketDataDelta;
  EventSeq source_sequence = 0;
  EventSeq insertion_sequence = 0;

  constexpr auto operator<=>(const EventOrderKey&) const noexcept = default;
};

struct InstrumentMeta {
  InstrumentId id = 0;
  std::array<char, 8> symbol{};
  // Tick size expressed in Price4 ($0.0001) units. e.g., 100 = $0.01, 1 = $0.0001.
  std::int64_t tick_size_price4 = 100;
  // Shares per canonical lot.
  std::uint64_t lot_size = 1;
  std::array<char, 4> currency{'U', 'S', 'D', '\0'};
  std::int64_t max_price_ticks = 10'000'000;
  std::uint64_t max_qty_lots = 1'000'000;

  static InstrumentMeta make(InstrumentId id, std::string_view sym,
                             std::int64_t tick_price4 = 100, std::uint64_t lot = 1,
                             std::string_view ccy = "USD") noexcept {
    InstrumentMeta m;
    m.id = id;
    m.symbol.fill(' ');
    const std::size_t sn = std::min(sym.size(), m.symbol.size());
    for (std::size_t i = 0; i < sn; ++i) m.symbol[i] = sym[i];
    m.tick_size_price4 = tick_price4;
    m.lot_size = lot;
    m.currency.fill('\0');
    const std::size_t cn = std::min<std::size_t>(ccy.size(), 3);
    for (std::size_t i = 0; i < cn; ++i) m.currency[i] = ccy[i];
    return m;
  }

  [[nodiscard]] std::string symbol_str() const {
    std::size_t end = symbol.size();
    while (end > 0 && (symbol[end - 1] == ' ' || symbol[end - 1] == '\0')) --end;
    return std::string(symbol.data(), end);
  }

  [[nodiscard]] constexpr bool valid_config() const noexcept {
    return tick_size_price4 > 0 && lot_size > 0 && max_price_ticks > 0 && max_qty_lots > 0;
  }

  // Checked conversion: canonical ticks -> Price4 ($0.0001) uint32_t (ITCH / OUCH wire).
  [[nodiscard]] constexpr std::optional<std::uint32_t> price_to_price4(Price p) const noexcept {
    if (!valid_config() || p.ticks <= 0 || p.ticks > max_price_ticks) return std::nullopt;
    const __int128 prod = static_cast<__int128>(p.ticks) * static_cast<__int128>(tick_size_price4);
    if (prod <= 0 || prod > static_cast<__int128>(std::numeric_limits<std::uint32_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(prod);
  }

  // Checked conversion: Price4 ($0.0001) -> canonical ticks (strict tick alignment required).
  [[nodiscard]] constexpr std::optional<Price> price4_to_price(std::uint64_t p4) const noexcept {
    if (!valid_config() || p4 == 0) return std::nullopt;
    const std::uint64_t ts = static_cast<std::uint64_t>(tick_size_price4);
    if (p4 % ts != 0) return std::nullopt;
    const std::uint64_t ticks = p4 / ts;
    if (ticks == 0 || ticks > static_cast<std::uint64_t>(max_price_ticks)) return std::nullopt;
    return Price{static_cast<std::int64_t>(ticks)};
  }

  // Checked conversion: canonical lots -> raw shares.
  [[nodiscard]] constexpr std::optional<std::uint64_t> qty_to_shares(Quantity q) const noexcept {
    if (!valid_config() || q.lots > max_qty_lots) return std::nullopt;
    const unsigned __int128 prod =
        static_cast<unsigned __int128>(q.lots) * static_cast<unsigned __int128>(lot_size);
    if (prod > static_cast<unsigned __int128>(std::numeric_limits<std::uint64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(prod);
  }

  // Checked conversion: raw shares -> canonical lots (strict lot alignment required).
  [[nodiscard]] constexpr std::optional<Quantity> shares_to_qty(std::uint64_t shares) const noexcept {
    if (!valid_config()) return std::nullopt;
    if (shares % lot_size != 0) return std::nullopt;
    const std::uint64_t lots = shares / lot_size;
    if (lots > max_qty_lots) return std::nullopt;
    return Quantity{lots};
  }

  // Exact fixed-point notional in Money ($0.0001 units): ticks * tick_size_price4 * lots * lot_size.
  [[nodiscard]] constexpr std::optional<Money> notional_scaled(Price p, Quantity q) const noexcept {
    if (!valid_config() || p.ticks < 0 || q.lots > max_qty_lots) return std::nullopt;
    const __int128 px4 = static_cast<__int128>(p.ticks) * static_cast<__int128>(tick_size_price4);
    const __int128 sh = static_cast<__int128>(q.lots) * static_cast<__int128>(lot_size);
    const __int128 tot = px4 * sh;
    if (tot < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
        tot > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return Money{static_cast<std::int64_t>(tot)};
  }

  // Exact integer fee computation from notional and fee_bps_x100 (e.g., 30 = 0.30 bps, -20 = -0.20 bps rebate).
  // Rounds half-away-from-zero to nearest $0.0001 scaled unit.
  [[nodiscard]] static constexpr std::optional<Money> compute_fee(
      Money notional, std::int32_t fee_bps_x100) noexcept {
    const __int128 num =
        static_cast<__int128>(notional.scaled) * static_cast<__int128>(fee_bps_x100);
    const __int128 den = static_cast<__int128>(kBpsScaleX100);
    const __int128 adjusted = (num >= 0) ? (num + den / 2) : (num - den / 2);
    const __int128 fee = adjusted / den;
    if (fee < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
        fee > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return Money{static_cast<std::int64_t>(fee)};
  }
};

struct VenueMeta {
  VenueId id = 0;
  std::string name = "VENUE0";
  // Hundredths of a basis point (e.g. -20 = -0.20 bps maker rebate, 30 = +0.30 bps taker fee).
  std::int32_t maker_fee_bps_x100 = -20;
  std::int32_t taker_fee_bps_x100 = 30;
  std::int64_t md_latency_ns = 50'000;        // 50 us
  std::int64_t order_latency_ns = 80'000;     // 80 us
  std::int64_t response_latency_ns = 60'000;  // 60 us
  std::int64_t stale_threshold_ns = 50'000'000;  // 50 ms
  bool enabled = true;
};

}  // namespace hft::contracts::v1
