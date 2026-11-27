#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "hft/contracts/v1/types.hpp"

namespace hft::contracts::v1 {

enum class MdUpdateKind : std::uint8_t {
  SnapshotClear = 0,
  SnapshotLevel = 1,
  Add = 2,
  Modify = 3,
  Cancel = 4,
  Delete = 5,
  Trade = 6,
  BookClear = 7,
};

enum class FeedHealth : std::uint8_t {
  Healthy = 0,
  GapDetected = 1,
  Stale = 2,
  Resyncing = 3,
};

struct MarketDataEvent {
  MdUpdateKind kind = MdUpdateKind::Add;
  std::uint16_t channel = 0;
  EventSeq feed_seq = 0;
  SimTimeNs source_time_ns{0};
  SimTimeNs receive_time_ns{0};
  FeedOrderRef feed_order_ref{0};
  Side side = Side::Buy;
  Price price{0};
  Quantity qty{0};
  FeedHealth health = FeedHealth::Healthy;
  bool is_snapshot = false;

  constexpr bool operator==(const MarketDataEvent&) const noexcept = default;
};

enum class OrderAction : std::uint8_t {
  New = 0,
  Cancel = 1,
  Replace = 2,
};

struct OrderIntent {
  OrderAction action = OrderAction::New;
  OrderId client_order_id = 0;
  ClientToken token{};
  ClientToken orig_token{};
  ParentOrderId parent_id = 0;
  Side side = Side::Buy;
  OrderType order_type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  bool post_only = false;
  Price limit_price{0};
  Quantity qty{0};
  Quantity remaining_qty_target{0};
  SimTimeNs strategy_decision_time_ns{0};

  constexpr bool operator==(const OrderIntent&) const noexcept = default;
};

enum class ExecReportType : std::uint8_t {
  Accepted = 0,
  Rejected = 1,
  PartiallyFilled = 2,
  Filled = 3,
  Canceled = 4,
  Replaced = 5,
  CancelRejected = 6,
  Uncertain = 7,
};

enum class RejectReason : std::uint16_t {
  None = 0,
  InvalidPrice = 1,
  InvalidTick = 2,
  InvalidQty = 3,
  DuplicateToken = 4,
  UnknownOrder = 5,
  UnknownSymbol = 6,
  RiskMaxOrderQty = 7,
  RiskMaxNotional = 8,
  RiskMaxPosition = 9,
  RiskMaxOpenOrders = 10,
  RiskStaleMarket = 11,
  RiskVenueDown = 12,
  RiskKillSwitch = 13,
  QueueFull = 14,
  SessionDisconnected = 15,
  ExchangeRejected = 16,
  PriceBand = 17,
  PostOnlyWouldCross = 18,
};

constexpr const char* to_string(ExecReportType t) noexcept {
  switch (t) {
    case ExecReportType::Accepted: return "ACCEPTED";
    case ExecReportType::Rejected: return "REJECTED";
    case ExecReportType::PartiallyFilled: return "PARTIAL_FILL";
    case ExecReportType::Filled: return "FILLED";
    case ExecReportType::Canceled: return "CANCELED";
    case ExecReportType::Replaced: return "REPLACED";
    case ExecReportType::CancelRejected: return "CANCEL_REJECTED";
    case ExecReportType::Uncertain: return "UNCERTAIN";
  }
  return "UNKNOWN";
}

constexpr const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None: return "NONE";
    case RejectReason::InvalidPrice: return "INVALID_PRICE";
    case RejectReason::InvalidTick: return "INVALID_TICK";
    case RejectReason::InvalidQty: return "INVALID_QTY";
    case RejectReason::DuplicateToken: return "DUPLICATE_TOKEN";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    case RejectReason::UnknownSymbol: return "UNKNOWN_SYMBOL";
    case RejectReason::RiskMaxOrderQty: return "RISK_MAX_ORDER_QTY";
    case RejectReason::RiskMaxNotional: return "RISK_MAX_NOTIONAL";
    case RejectReason::RiskMaxPosition: return "RISK_MAX_POSITION";
    case RejectReason::RiskMaxOpenOrders: return "RISK_MAX_OPEN_ORDERS";
    case RejectReason::RiskStaleMarket: return "RISK_STALE_MARKET";
    case RejectReason::RiskVenueDown: return "RISK_VENUE_DOWN";
    case RejectReason::RiskKillSwitch: return "RISK_KILL_SWITCH";
    case RejectReason::QueueFull: return "QUEUE_FULL";
    case RejectReason::SessionDisconnected: return "SESSION_DISCONNECTED";
    case RejectReason::ExchangeRejected: return "EXCHANGE_REJECTED";
    case RejectReason::PriceBand: return "PRICE_BAND";
    case RejectReason::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
  }
  return "UNKNOWN";
}

struct ExecutionReport {
  ExecReportType type = ExecReportType::Accepted;
  OrderId client_order_id = 0;
  ClientToken token{};
  ExchangeOrderId exchange_order_id = 0;
  ExecId exec_id = 0;
  Side side = Side::Buy;
  Price exec_price{0};
  Quantity requested_qty{0};
  Quantity fill_qty{0};
  Quantity cum_filled_qty{0};
  Quantity leaves_qty{0};
  Money fee_scaled{0};
  bool is_maker = false;
  RejectReason reject_reason = RejectReason::None;
  SimTimeNs exchange_time_ns{0};
  SimTimeNs gateway_recv_time_ns{0};

  constexpr bool operator==(const ExecutionReport&) const noexcept = default;
};

struct SystemEvent {
  enum class Kind : std::uint8_t {
    SessionUp = 0,
    SessionDown = 1,
    VenueDown = 2,
    VenueUp = 3,
    KillSwitchTriggered = 4,
    FeedGap = 5,
    FeedRecovered = 6,
    AuditNote = 7,
  };

  Kind kind = Kind::AuditNote;
  ErrorCode error = ErrorCode::Ok;
  ErrorClass error_class = ErrorClass::None;
  std::uint64_t detail_code = 0;
  std::array<char, 64> message{};

  static SystemEvent make(Kind k, ErrorCode err, std::uint64_t detail,
                          std::string_view msg) noexcept {
    SystemEvent e;
    e.kind = k;
    e.error = err;
    e.error_class = classify_error(err);
    e.detail_code = detail;
    e.message.fill('\0');
    const std::size_t n = std::min<std::size_t>(msg.size(), e.message.size() - 1);
    for (std::size_t i = 0; i < n; ++i) e.message[i] = msg[i];
    return e;
  }

  constexpr bool operator==(const SystemEvent&) const noexcept = default;
};

using EnvelopePayload =
    std::variant<MarketDataEvent, OrderIntent, ExecutionReport, SystemEvent>;

struct Envelope {
  std::uint32_t contract_version = kContractVersion;
  EventSeq sequence = 0;
  SimTimeNs event_time_ns{0};
  PriorityClass priority = PriorityClass::MarketDataDelta;
  InstrumentId instrument = 0;
  VenueId venue = 0;
  EnvelopePayload payload{};

  constexpr bool operator==(const Envelope&) const noexcept = default;
};

// Fixed-size wire encoding/decoding for Envelope (contracts/v1) to guarantee deterministic
// serialization, endianness stability, and strict contract version validation.
inline constexpr std::uint32_t kEnvelopeMagic = 0x48465431U;  // "HFT1"
inline constexpr std::size_t kEnvelopeHeaderSize = 36;

namespace detail {

inline void write_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
inline void write_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}
inline void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8U)) & 0xFFU));
}
inline void write_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (i * 8U)) & 0xFFU));
}
inline void write_i64(std::vector<std::uint8_t>& out, std::int64_t v) {
  write_u64(out, static_cast<std::uint64_t>(v));
}

inline bool read_u8(std::span<const std::uint8_t> in, std::size_t& off, std::uint8_t& v) noexcept {
  if (off + 1 > in.size()) return false;
  v = in[off++];
  return true;
}
inline bool read_u16(std::span<const std::uint8_t> in, std::size_t& off, std::uint16_t& v) noexcept {
  if (off + 2 > in.size()) return false;
  v = static_cast<std::uint16_t>(in[off]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[off + 1]) << 8U);
  off += 2;
  return true;
}
inline bool read_u32(std::span<const std::uint8_t> in, std::size_t& off, std::uint32_t& v) noexcept {
  if (off + 4 > in.size()) return false;
  v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[off + i]) << (i * 8U);
  off += 4;
  return true;
}
inline bool read_u64(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v) noexcept {
  if (off + 8 > in.size()) return false;
  v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[off + i]) << (i * 8U);
  off += 8;
  return true;
}
inline bool read_i64(std::span<const std::uint8_t> in, std::size_t& off, std::int64_t& v) noexcept {
  std::uint64_t raw = 0;
  if (!read_u64(in, off, raw)) return false;
  v = static_cast<std::int64_t>(raw);
  return true;
}

inline std::uint64_t fnv1a_update(std::uint64_t h, std::span<const std::uint8_t> bytes) noexcept {
  for (std::uint8_t b : bytes) {
    h ^= static_cast<std::uint64_t>(b);
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace detail

inline std::vector<std::uint8_t> encode_envelope(const Envelope& env) {
  std::vector<std::uint8_t> out;
  out.reserve(160);
  detail::write_u32(out, kEnvelopeMagic);
  detail::write_u32(out, env.contract_version);
  detail::write_u64(out, env.sequence);
  detail::write_i64(out, env.event_time_ns.value);
  detail::write_u8(out, static_cast<std::uint8_t>(env.priority));
  detail::write_u8(out, static_cast<std::uint8_t>(env.payload.index()));
  detail::write_u16(out, env.venue);
  detail::write_u32(out, env.instrument);
  detail::write_u32(out, 0U);  // reserved

  if (const auto* md = std::get_if<MarketDataEvent>(&env.payload)) {
    detail::write_u8(out, static_cast<std::uint8_t>(md->kind));
    detail::write_u8(out, static_cast<std::uint8_t>(md->side));
    detail::write_u8(out, static_cast<std::uint8_t>(md->health));
    detail::write_u8(out, md->is_snapshot ? 1U : 0U);
    detail::write_u16(out, md->channel);
    detail::write_u64(out, md->feed_seq);
    detail::write_i64(out, md->source_time_ns.value);
    detail::write_i64(out, md->receive_time_ns.value);
    detail::write_u64(out, md->feed_order_ref.value);
    detail::write_i64(out, md->price.ticks);
    detail::write_u64(out, md->qty.lots);
  } else if (const auto* oi = std::get_if<OrderIntent>(&env.payload)) {
    detail::write_u8(out, static_cast<std::uint8_t>(oi->action));
    detail::write_u8(out, static_cast<std::uint8_t>(oi->side));
    detail::write_u8(out, static_cast<std::uint8_t>(oi->order_type));
    detail::write_u8(out, static_cast<std::uint8_t>(oi->tif));
    detail::write_u8(out, oi->post_only ? 1U : 0U);
    detail::write_u64(out, oi->client_order_id);
    for (char c : oi->token.bytes) detail::write_u8(out, static_cast<std::uint8_t>(c));
    for (char c : oi->orig_token.bytes) detail::write_u8(out, static_cast<std::uint8_t>(c));
    detail::write_u64(out, oi->parent_id);
    detail::write_i64(out, oi->limit_price.ticks);
    detail::write_u64(out, oi->qty.lots);
    detail::write_u64(out, oi->remaining_qty_target.lots);
    detail::write_i64(out, oi->strategy_decision_time_ns.value);
  } else if (const auto* er = std::get_if<ExecutionReport>(&env.payload)) {
    detail::write_u8(out, static_cast<std::uint8_t>(er->type));
    detail::write_u8(out, static_cast<std::uint8_t>(er->side));
    detail::write_u8(out, er->is_maker ? 1U : 0U);
    detail::write_u16(out, static_cast<std::uint16_t>(er->reject_reason));
    detail::write_u64(out, er->client_order_id);
    for (char c : er->token.bytes) detail::write_u8(out, static_cast<std::uint8_t>(c));
    detail::write_u64(out, er->exchange_order_id);
    detail::write_u64(out, er->exec_id);
    detail::write_i64(out, er->exec_price.ticks);
    detail::write_u64(out, er->requested_qty.lots);
    detail::write_u64(out, er->fill_qty.lots);
    detail::write_u64(out, er->cum_filled_qty.lots);
    detail::write_u64(out, er->leaves_qty.lots);
    detail::write_i64(out, er->fee_scaled.scaled);
    detail::write_i64(out, er->exchange_time_ns.value);
    detail::write_i64(out, er->gateway_recv_time_ns.value);
  } else if (const auto* se = std::get_if<SystemEvent>(&env.payload)) {
    detail::write_u8(out, static_cast<std::uint8_t>(se->kind));
    detail::write_u16(out, static_cast<std::uint16_t>(se->error));
    detail::write_u8(out, static_cast<std::uint8_t>(se->error_class));
    detail::write_u64(out, se->detail_code);
    for (char c : se->message) detail::write_u8(out, static_cast<std::uint8_t>(c));
  }
  return out;
}

inline ErrorCode decode_envelope(std::span<const std::uint8_t> bytes, Envelope& out) noexcept {
  std::size_t off = 0;
  std::uint32_t magic = 0;
  std::uint32_t ver = 0;
  std::uint8_t prio = 0;
  std::uint8_t payload_kind = 0;
  std::uint32_t reserved = 0;
  if (!detail::read_u32(bytes, off, magic) || !detail::read_u32(bytes, off, ver) ||
      !detail::read_u64(bytes, off, out.sequence) ||
      !detail::read_i64(bytes, off, out.event_time_ns.value) ||
      !detail::read_u8(bytes, off, prio) || !detail::read_u8(bytes, off, payload_kind) ||
      !detail::read_u16(bytes, off, out.venue) || !detail::read_u32(bytes, off, out.instrument) ||
      !detail::read_u32(bytes, off, reserved)) {
    return ErrorCode::DecodeError;
  }
  if (magic != kEnvelopeMagic) return ErrorCode::DecodeError;
  if ((ver >> 16U) != kContractVersionMajor) return ErrorCode::VersionMismatch;
  if (prio > static_cast<std::uint8_t>(PriorityClass::Timer)) return ErrorCode::DecodeError;
  out.contract_version = ver;
  out.priority = static_cast<PriorityClass>(prio);

  if (payload_kind == 0) {
    MarketDataEvent md{};
    std::uint8_t k = 0, s = 0, h = 0, snap = 0;
    if (!detail::read_u8(bytes, off, k) || !detail::read_u8(bytes, off, s) ||
        !detail::read_u8(bytes, off, h) || !detail::read_u8(bytes, off, snap) ||
        !detail::read_u16(bytes, off, md.channel) || !detail::read_u64(bytes, off, md.feed_seq) ||
        !detail::read_i64(bytes, off, md.source_time_ns.value) ||
        !detail::read_i64(bytes, off, md.receive_time_ns.value) ||
        !detail::read_u64(bytes, off, md.feed_order_ref.value) ||
        !detail::read_i64(bytes, off, md.price.ticks) ||
        !detail::read_u64(bytes, off, md.qty.lots)) {
      return ErrorCode::DecodeError;
    }
    md.kind = static_cast<MdUpdateKind>(k);
    md.side = static_cast<Side>(s);
    md.health = static_cast<FeedHealth>(h);
    md.is_snapshot = (snap != 0);
    out.payload = md;
  } else if (payload_kind == 1) {
    OrderIntent oi{};
    std::uint8_t a = 0, s = 0, ot = 0, tif = 0, po = 0;
    if (!detail::read_u8(bytes, off, a) || !detail::read_u8(bytes, off, s) ||
        !detail::read_u8(bytes, off, ot) || !detail::read_u8(bytes, off, tif) ||
        !detail::read_u8(bytes, off, po) || !detail::read_u64(bytes, off, oi.client_order_id)) {
      return ErrorCode::DecodeError;
    }
    if (off + 28 > bytes.size()) return ErrorCode::DecodeError;
    for (std::size_t i = 0; i < 14; ++i) oi.token.bytes[i] = static_cast<char>(bytes[off++]);
    for (std::size_t i = 0; i < 14; ++i) oi.orig_token.bytes[i] = static_cast<char>(bytes[off++]);
    if (!detail::read_u64(bytes, off, oi.parent_id) ||
        !detail::read_i64(bytes, off, oi.limit_price.ticks) ||
        !detail::read_u64(bytes, off, oi.qty.lots) ||
        !detail::read_u64(bytes, off, oi.remaining_qty_target.lots) ||
        !detail::read_i64(bytes, off, oi.strategy_decision_time_ns.value)) {
      return ErrorCode::DecodeError;
    }
    oi.action = static_cast<OrderAction>(a);
    oi.side = static_cast<Side>(s);
    oi.order_type = static_cast<OrderType>(ot);
    oi.tif = static_cast<TimeInForce>(tif);
    oi.post_only = (po != 0);
    out.payload = oi;
  } else if (payload_kind == 2) {
    ExecutionReport er{};
    std::uint8_t t = 0, s = 0, mk = 0;
    std::uint16_t rr = 0;
    if (!detail::read_u8(bytes, off, t) || !detail::read_u8(bytes, off, s) ||
        !detail::read_u8(bytes, off, mk) || !detail::read_u16(bytes, off, rr) ||
        !detail::read_u64(bytes, off, er.client_order_id)) {
      return ErrorCode::DecodeError;
    }
    if (off + 14 > bytes.size()) return ErrorCode::DecodeError;
    for (std::size_t i = 0; i < 14; ++i) er.token.bytes[i] = static_cast<char>(bytes[off++]);
    if (!detail::read_u64(bytes, off, er.exchange_order_id) ||
        !detail::read_u64(bytes, off, er.exec_id) ||
        !detail::read_i64(bytes, off, er.exec_price.ticks) ||
        !detail::read_u64(bytes, off, er.requested_qty.lots) ||
        !detail::read_u64(bytes, off, er.fill_qty.lots) ||
        !detail::read_u64(bytes, off, er.cum_filled_qty.lots) ||
        !detail::read_u64(bytes, off, er.leaves_qty.lots) ||
        !detail::read_i64(bytes, off, er.fee_scaled.scaled) ||
        !detail::read_i64(bytes, off, er.exchange_time_ns.value) ||
        !detail::read_i64(bytes, off, er.gateway_recv_time_ns.value)) {
      return ErrorCode::DecodeError;
    }
    er.type = static_cast<ExecReportType>(t);
    er.side = static_cast<Side>(s);
    er.is_maker = (mk != 0);
    er.reject_reason = static_cast<RejectReason>(rr);
    out.payload = er;
  } else if (payload_kind == 3) {
    SystemEvent se{};
    std::uint8_t k = 0, ec = 0;
    std::uint16_t err = 0;
    if (!detail::read_u8(bytes, off, k) || !detail::read_u16(bytes, off, err) ||
        !detail::read_u8(bytes, off, ec) || !detail::read_u64(bytes, off, se.detail_code)) {
      return ErrorCode::DecodeError;
    }
    if (off + 64 > bytes.size()) return ErrorCode::DecodeError;
    for (std::size_t i = 0; i < 64; ++i) se.message[i] = static_cast<char>(bytes[off++]);
    se.kind = static_cast<SystemEvent::Kind>(k);
    se.error = static_cast<ErrorCode>(err);
    se.error_class = static_cast<ErrorClass>(ec);
    out.payload = se;
  } else {
    return ErrorCode::DecodeError;
  }

  return (off == bytes.size()) ? ErrorCode::Ok : ErrorCode::DecodeError;
}

inline std::uint64_t hash_envelope(const Envelope& env,
                                   std::uint64_t seed = 14695981039346656037ULL) {
  const auto bytes = encode_envelope(env);
  return detail::fnv1a_update(seed, bytes);
}

}  // namespace hft::contracts::v1
