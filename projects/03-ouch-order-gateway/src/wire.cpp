#include "ouch/wire.hpp"

#include <cstring>

namespace ouch::wire {
namespace {

class Writer {
 public:
  explicit Writer(std::uint8_t* p) : p_(p) {}
  void ch(char v) { *p_++ = static_cast<std::uint8_t>(v); }
  void u32(std::uint32_t v) {
    p_[0] = static_cast<std::uint8_t>(v >> 24);
    p_[1] = static_cast<std::uint8_t>(v >> 16);
    p_[2] = static_cast<std::uint8_t>(v >> 8);
    p_[3] = static_cast<std::uint8_t>(v);
    p_ += 4;
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v >> 32));
    u32(static_cast<std::uint32_t>(v));
  }
  template <std::size_t N>
  void alpha(const Alpha<N>& a) {
    std::memcpy(p_, a.c.data(), N);
    p_ += N;
  }

 private:
  std::uint8_t* p_;
};

class Reader {
 public:
  explicit Reader(const std::uint8_t* p) : p_(p) {}
  char ch() { return static_cast<char>(*p_++); }
  std::uint32_t u32() {
    std::uint32_t v = (static_cast<std::uint32_t>(p_[0]) << 24) |
                      (static_cast<std::uint32_t>(p_[1]) << 16) |
                      (static_cast<std::uint32_t>(p_[2]) << 8) | static_cast<std::uint32_t>(p_[3]);
    p_ += 4;
    return v;
  }
  std::uint64_t u64() {
    std::uint64_t hi = u32();
    return (hi << 32) | u32();
  }
  template <std::size_t N>
  Alpha<N> alpha() {
    Alpha<N> a;
    std::memcpy(a.c.data(), p_, N);
    p_ += N;
    return a;
  }

 private:
  const std::uint8_t* p_;
};

template <typename M>
bool fits(std::span<std::uint8_t> out) {
  return out.size() >= M::kSize;
}

EnterOrder read_enter(Reader r) {
  EnterOrder m;
  r.ch();
  m.token = r.alpha<14>();
  m.side = r.ch();
  m.shares = r.u32();
  m.stock = r.alpha<8>();
  m.price = r.u32();
  m.tif = r.u32();
  m.firm = r.alpha<4>();
  m.display = r.ch();
  m.capacity = r.ch();
  m.iso = r.ch();
  m.min_qty = r.u32();
  m.cross_type = r.ch();
  m.customer_type = r.ch();
  return m;
}

ReplaceOrder read_replace(Reader r) {
  ReplaceOrder m;
  r.ch();
  m.existing_token = r.alpha<14>();
  m.replacement_token = r.alpha<14>();
  m.shares = r.u32();
  m.price = r.u32();
  m.tif = r.u32();
  m.display = r.ch();
  m.iso = r.ch();
  m.min_qty = r.u32();
  return m;
}

CancelOrder read_cancel(Reader r) {
  CancelOrder m;
  r.ch();
  m.token = r.alpha<14>();
  m.shares = r.u32();
  return m;
}

SystemEvent read_system(Reader r) {
  SystemEvent m;
  r.ch();
  m.timestamp = r.u64();
  m.event_code = r.ch();
  return m;
}

Accepted read_accepted(Reader r) {
  Accepted m;
  r.ch();
  m.timestamp = r.u64();
  m.token = r.alpha<14>();
  m.side = r.ch();
  m.shares = r.u32();
  m.stock = r.alpha<8>();
  m.price = r.u32();
  m.tif = r.u32();
  m.firm = r.alpha<4>();
  m.display = r.ch();
  m.order_ref = r.u64();
  m.capacity = r.ch();
  m.iso = r.ch();
  m.min_qty = r.u32();
  m.cross_type = r.ch();
  m.order_state = r.ch();
  m.bbo_weight = r.ch();
  return m;
}

Replaced read_replaced(Reader r) {
  Replaced m;
  r.ch();
  m.timestamp = r.u64();
  m.token = r.alpha<14>();
  m.side = r.ch();
  m.shares = r.u32();
  m.stock = r.alpha<8>();
  m.price = r.u32();
  m.tif = r.u32();
  m.firm = r.alpha<4>();
  m.display = r.ch();
  m.order_ref = r.u64();
  m.capacity = r.ch();
  m.iso = r.ch();
  m.min_qty = r.u32();
  m.cross_type = r.ch();
  m.order_state = r.ch();
  m.previous_token = r.alpha<14>();
  m.bbo_weight = r.ch();
  return m;
}

Canceled read_canceled(Reader r) {
  Canceled m;
  r.ch();
  m.timestamp = r.u64();
  m.token = r.alpha<14>();
  m.decrement_shares = r.u32();
  m.reason = r.ch();
  return m;
}

Executed read_executed(Reader r) {
  Executed m;
  r.ch();
  m.timestamp = r.u64();
  m.token = r.alpha<14>();
  m.shares = r.u32();
  m.price = r.u32();
  m.liquidity = r.ch();
  m.match_number = r.u64();
  return m;
}

Rejected read_rejected(Reader r) {
  Rejected m;
  r.ch();
  m.timestamp = r.u64();
  m.token = r.alpha<14>();
  m.reason = r.ch();
  return m;
}

bool valid_price(std::uint32_t price) {
  return price == kMarketPrice || (price > 0 && price <= kMaxLimitPrice);
}

bool valid_display(char d) {
  switch (d) {
    case 'A':  // attributable
    case 'Y':  // anonymous-price to comply
    case 'N':  // non-display
    case 'P':  // post only
    case 'I':  // imbalance only
    case 'M':  // mid-point peg
    case 'W':  // mid-point peg post only
    case 'L':  // post-only and attributable
    case 'O':  // retail order type 1
    case 'T':  // retail order type 2
    case 'Q':  // retail price improvement
      return true;
    default:
      return false;
  }
}

}  // namespace

const char* to_string(DecodeError e) {
  switch (e) {
    case DecodeError::None:
      return "none";
    case DecodeError::Empty:
      return "empty";
    case DecodeError::UnknownType:
      return "unknown-type";
    case DecodeError::BadLength:
      return "bad-length";
  }
  return "?";
}

std::size_t encode(const EnterOrder& m, std::span<std::uint8_t> out) {
  if (!fits<EnterOrder>(out)) return 0;
  Writer w(out.data());
  w.ch(EnterOrder::kType);
  w.alpha(m.token);
  w.ch(m.side);
  w.u32(m.shares);
  w.alpha(m.stock);
  w.u32(m.price);
  w.u32(m.tif);
  w.alpha(m.firm);
  w.ch(m.display);
  w.ch(m.capacity);
  w.ch(m.iso);
  w.u32(m.min_qty);
  w.ch(m.cross_type);
  w.ch(m.customer_type);
  return EnterOrder::kSize;
}

std::size_t encode(const ReplaceOrder& m, std::span<std::uint8_t> out) {
  if (!fits<ReplaceOrder>(out)) return 0;
  Writer w(out.data());
  w.ch(ReplaceOrder::kType);
  w.alpha(m.existing_token);
  w.alpha(m.replacement_token);
  w.u32(m.shares);
  w.u32(m.price);
  w.u32(m.tif);
  w.ch(m.display);
  w.ch(m.iso);
  w.u32(m.min_qty);
  return ReplaceOrder::kSize;
}

std::size_t encode(const CancelOrder& m, std::span<std::uint8_t> out) {
  if (!fits<CancelOrder>(out)) return 0;
  Writer w(out.data());
  w.ch(CancelOrder::kType);
  w.alpha(m.token);
  w.u32(m.shares);
  return CancelOrder::kSize;
}

std::size_t encode(const SystemEvent& m, std::span<std::uint8_t> out) {
  if (!fits<SystemEvent>(out)) return 0;
  Writer w(out.data());
  w.ch(SystemEvent::kType);
  w.u64(m.timestamp);
  w.ch(m.event_code);
  return SystemEvent::kSize;
}

std::size_t encode(const Accepted& m, std::span<std::uint8_t> out) {
  if (!fits<Accepted>(out)) return 0;
  Writer w(out.data());
  w.ch(Accepted::kType);
  w.u64(m.timestamp);
  w.alpha(m.token);
  w.ch(m.side);
  w.u32(m.shares);
  w.alpha(m.stock);
  w.u32(m.price);
  w.u32(m.tif);
  w.alpha(m.firm);
  w.ch(m.display);
  w.u64(m.order_ref);
  w.ch(m.capacity);
  w.ch(m.iso);
  w.u32(m.min_qty);
  w.ch(m.cross_type);
  w.ch(m.order_state);
  w.ch(m.bbo_weight);
  return Accepted::kSize;
}

std::size_t encode(const Replaced& m, std::span<std::uint8_t> out) {
  if (!fits<Replaced>(out)) return 0;
  Writer w(out.data());
  w.ch(Replaced::kType);
  w.u64(m.timestamp);
  w.alpha(m.token);
  w.ch(m.side);
  w.u32(m.shares);
  w.alpha(m.stock);
  w.u32(m.price);
  w.u32(m.tif);
  w.alpha(m.firm);
  w.ch(m.display);
  w.u64(m.order_ref);
  w.ch(m.capacity);
  w.ch(m.iso);
  w.u32(m.min_qty);
  w.ch(m.cross_type);
  w.ch(m.order_state);
  w.alpha(m.previous_token);
  w.ch(m.bbo_weight);
  return Replaced::kSize;
}

std::size_t encode(const Canceled& m, std::span<std::uint8_t> out) {
  if (!fits<Canceled>(out)) return 0;
  Writer w(out.data());
  w.ch(Canceled::kType);
  w.u64(m.timestamp);
  w.alpha(m.token);
  w.u32(m.decrement_shares);
  w.ch(m.reason);
  return Canceled::kSize;
}

std::size_t encode(const Executed& m, std::span<std::uint8_t> out) {
  if (!fits<Executed>(out)) return 0;
  Writer w(out.data());
  w.ch(Executed::kType);
  w.u64(m.timestamp);
  w.alpha(m.token);
  w.u32(m.shares);
  w.u32(m.price);
  w.ch(m.liquidity);
  w.u64(m.match_number);
  return Executed::kSize;
}

std::size_t encode(const Rejected& m, std::span<std::uint8_t> out) {
  if (!fits<Rejected>(out)) return 0;
  Writer w(out.data());
  w.ch(Rejected::kType);
  w.u64(m.timestamp);
  w.alpha(m.token);
  w.ch(m.reason);
  return Rejected::kSize;
}

std::size_t encode(const InboundMessage& m, std::span<std::uint8_t> out) {
  return std::visit([&](const auto& x) { return encode(x, out); }, m);
}

std::size_t encode(const OutboundMessage& m, std::span<std::uint8_t> out) {
  return std::visit([&](const auto& x) { return encode(x, out); }, m);
}

DecodeError decode_inbound(std::span<const std::uint8_t> in, InboundMessage& out) {
  if (in.empty()) return DecodeError::Empty;
  Reader r(in.data());
  switch (static_cast<char>(in[0])) {
    case EnterOrder::kType:
      if (in.size() != EnterOrder::kSize) return DecodeError::BadLength;
      out = read_enter(r);
      return DecodeError::None;
    case ReplaceOrder::kType:
      if (in.size() != ReplaceOrder::kSize) return DecodeError::BadLength;
      out = read_replace(r);
      return DecodeError::None;
    case CancelOrder::kType:
      if (in.size() != CancelOrder::kSize) return DecodeError::BadLength;
      out = read_cancel(r);
      return DecodeError::None;
    default:
      return DecodeError::UnknownType;
  }
}

DecodeError decode_outbound(std::span<const std::uint8_t> in, OutboundMessage& out) {
  if (in.empty()) return DecodeError::Empty;
  Reader r(in.data());
  switch (static_cast<char>(in[0])) {
    case SystemEvent::kType:
      if (in.size() != SystemEvent::kSize) return DecodeError::BadLength;
      out = read_system(r);
      return DecodeError::None;
    case Accepted::kType:
      if (in.size() != Accepted::kSize) return DecodeError::BadLength;
      out = read_accepted(r);
      return DecodeError::None;
    case Replaced::kType:
      if (in.size() != Replaced::kSize) return DecodeError::BadLength;
      out = read_replaced(r);
      return DecodeError::None;
    case Canceled::kType:
      if (in.size() != Canceled::kSize) return DecodeError::BadLength;
      out = read_canceled(r);
      return DecodeError::None;
    case Executed::kType:
      if (in.size() != Executed::kSize) return DecodeError::BadLength;
      out = read_executed(r);
      return DecodeError::None;
    case Rejected::kType:
      if (in.size() != Rejected::kSize) return DecodeError::BadLength;
      out = read_rejected(r);
      return DecodeError::None;
    default:
      return DecodeError::UnknownType;
  }
}

char validate(const EnterOrder& m) {
  if (m.token.blank()) return 'O';
  if (!is_valid_side(m.side)) return 'O';
  if (m.shares == 0 || m.shares >= kMaxShares) return 'Z';
  if (m.stock.blank()) return 'S';
  if (!valid_price(m.price)) return 'X';
  if (!valid_display(m.display)) return 'D';
  if (m.min_qty > m.shares) return 'N';
  return 0;
}

char validate(const ReplaceOrder& m) {
  if (m.replacement_token.blank()) return 'O';
  if (m.shares == 0 || m.shares >= kMaxShares) return 'Z';
  if (!valid_price(m.price)) return 'X';
  if (!valid_display(m.display)) return 'D';
  if (m.min_qty > m.shares) return 'N';
  return 0;
}

}  // namespace ouch::wire
