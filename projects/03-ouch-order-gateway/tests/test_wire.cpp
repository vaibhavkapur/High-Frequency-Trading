// Wire fixtures: every supported message is laid out byte by byte from the
// field tables in docs/protocol.md, independently of the encoder, and must
// decode to the expected fields and re-encode to identical bytes.
#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <vector>

#include "ouch/wire.hpp"

using namespace ouch;
using namespace ouch::wire;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Builds a fixture from ASCII runs and literal bytes.
class Fixture {
 public:
  Fixture& a(std::string_view s) {
    for (char c : s) b_.push_back(static_cast<std::uint8_t>(c));
    return *this;
  }
  Fixture& x(std::initializer_list<int> bytes) {
    for (int v : bytes) b_.push_back(static_cast<std::uint8_t>(v));
    return *this;
  }
  [[nodiscard]] const Bytes& bytes() const { return b_; }

 private:
  Bytes b_;
};

template <typename M>
Bytes encoded(const M& m) {
  Bytes out(M::kSize);
  EXPECT_EQ(encode(m, out), M::kSize);
  return out;
}

template <typename M>
M decode_in(const Bytes& b) {
  InboundMessage v;
  EXPECT_EQ(decode_inbound(b, v), DecodeError::None);
  EXPECT_TRUE(std::holds_alternative<M>(v));
  return std::get<M>(v);
}

template <typename M>
M decode_out(const Bytes& b) {
  OutboundMessage v;
  EXPECT_EQ(decode_outbound(b, v), DecodeError::None);
  EXPECT_TRUE(std::holds_alternative<M>(v));
  return std::get<M>(v);
}

// 12:34:56.789012345 after midnight = 45296789012345 ns = 0x29327b04bf79.
const std::initializer_list<int> kTimestamp = {0x00, 0x00, 0x29, 0x32, 0x7B, 0x04, 0xBF, 0x79};
constexpr std::uint64_t kTimestampNs = 45'296'789'012'345ULL;

}  // namespace

TEST(WireFixture, TimestampConstantIsConsistent) {
  std::uint64_t v = 0;
  for (int b : kTimestamp) v = (v << 8) | static_cast<std::uint64_t>(b);
  EXPECT_EQ(v, kTimestampNs);
}

TEST(WireFixture, EnterOrder) {
  const Bytes fx = Fixture()
                       .a("O")
                       .a("TOKEN000000001")
                       .a("B")
                       .x({0x00, 0x00, 0x00, 0x64})  // 100 shares
                       .a("AAPL    ")
                       .x({0x00, 0x16, 0xE3, 0x60})  // 150.0000
                       .x({0x00, 0x01, 0x86, 0x9E})  // 99998 = market hours
                       .a("FIRM")
                       .a("Y")
                       .a("A")
                       .a("N")
                       .x({0x00, 0x00, 0x00, 0x00})
                       .a("N")
                       .a(" ")
                       .bytes();
  ASSERT_EQ(fx.size(), EnterOrder::kSize);
  const auto m = decode_in<EnterOrder>(fx);
  EXPECT_EQ(m.token.str(), "TOKEN000000001");
  EXPECT_EQ(m.side, 'B');
  EXPECT_EQ(m.shares, 100u);
  EXPECT_EQ(m.stock.str(), "AAPL");
  EXPECT_EQ(m.price, 1'500'000u);
  EXPECT_EQ(m.tif, kTifMarketHours);
  EXPECT_EQ(m.firm.str(), "FIRM");
  EXPECT_EQ(m.display, 'Y');
  EXPECT_EQ(m.capacity, 'A');
  EXPECT_EQ(m.iso, 'N');
  EXPECT_EQ(m.min_qty, 0u);
  EXPECT_EQ(m.cross_type, 'N');
  EXPECT_EQ(m.customer_type, ' ');
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, ReplaceOrder) {
  const Bytes fx = Fixture()
                       .a("U")
                       .a("TOKEN000000001")
                       .a("TOKEN000000002")
                       .x({0x00, 0x00, 0x01, 0x2C})  // 300 shares
                       .x({0x00, 0x16, 0xE7, 0x48})  // 150.1000
                       .x({0x00, 0x00, 0x00, 0x00})  // IOC
                       .a("N")
                       .a("Y")
                       .x({0x00, 0x00, 0x00, 0x0A})  // min qty 10
                       .bytes();
  ASSERT_EQ(fx.size(), ReplaceOrder::kSize);
  const auto m = decode_in<ReplaceOrder>(fx);
  EXPECT_EQ(m.existing_token.str(), "TOKEN000000001");
  EXPECT_EQ(m.replacement_token.str(), "TOKEN000000002");
  EXPECT_EQ(m.shares, 300u);
  EXPECT_EQ(m.price, 1'501'000u);
  EXPECT_EQ(m.tif, kTifIoc);
  EXPECT_EQ(m.display, 'N');
  EXPECT_EQ(m.iso, 'Y');
  EXPECT_EQ(m.min_qty, 10u);
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, CancelOrder) {
  const Bytes fx = Fixture().a("X").a("TOKEN000000001").x({0x00, 0x00, 0x00, 0x00}).bytes();
  ASSERT_EQ(fx.size(), CancelOrder::kSize);
  const auto m = decode_in<CancelOrder>(fx);
  EXPECT_EQ(m.token.str(), "TOKEN000000001");
  EXPECT_EQ(m.shares, 0u);
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, SystemEvent) {
  const Bytes fx = Fixture().a("S").x(kTimestamp).a("S").bytes();
  ASSERT_EQ(fx.size(), SystemEvent::kSize);
  const auto m = decode_out<SystemEvent>(fx);
  EXPECT_EQ(m.timestamp, kTimestampNs);
  EXPECT_EQ(m.event_code, 'S');
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, Accepted) {
  const Bytes fx = Fixture()
                       .a("A")
                       .x(kTimestamp)
                       .a("TOKEN000000001")
                       .a("S")
                       .x({0x00, 0x00, 0x03, 0xE8})  // 1000 shares
                       .a("MSFT    ")
                       .x({0x00, 0x3D, 0x09, 0x00})  // 400.0000
                       .x({0x00, 0x01, 0x86, 0x9F})  // 99999 = system hours
                       .a("ABCD")
                       .a("A")
                       .x({0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2A})  // ref 2^32 + 42
                       .a("P")
                       .a("N")
                       .x({0x00, 0x00, 0x00, 0x64})  // min qty 100
                       .a("N")
                       .a("L")
                       .a("2")
                       .bytes();
  ASSERT_EQ(fx.size(), Accepted::kSize);
  const auto m = decode_out<Accepted>(fx);
  EXPECT_EQ(m.timestamp, kTimestampNs);
  EXPECT_EQ(m.token.str(), "TOKEN000000001");
  EXPECT_EQ(m.side, 'S');
  EXPECT_EQ(m.shares, 1000u);
  EXPECT_EQ(m.stock.str(), "MSFT");
  EXPECT_EQ(m.price, 4'000'000u);
  EXPECT_EQ(m.tif, kTifSystemHours);
  EXPECT_EQ(m.firm.str(), "ABCD");
  EXPECT_EQ(m.display, 'A');
  EXPECT_EQ(m.order_ref, (1ULL << 32) + 42);
  EXPECT_EQ(m.capacity, 'P');
  EXPECT_EQ(m.iso, 'N');
  EXPECT_EQ(m.min_qty, 100u);
  EXPECT_EQ(m.cross_type, 'N');
  EXPECT_EQ(m.order_state, 'L');
  EXPECT_EQ(m.bbo_weight, '2');
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, Replaced) {
  const Bytes fx = Fixture()
                       .a("U")
                       .x(kTimestamp)
                       .a("TOKEN000000002")
                       .a("B")
                       .x({0x00, 0x00, 0x00, 0xC8})  // 200 shares
                       .a("AAPL    ")
                       .x({0x00, 0x16, 0xE7, 0x48})  // 150.1000
                       .x({0x00, 0x01, 0x86, 0x9E})
                       .a("FIRM")
                       .a("Y")
                       .x({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07})
                       .a("A")
                       .a("N")
                       .x({0x00, 0x00, 0x00, 0x00})
                       .a("N")
                       .a("L")
                       .a("TOKEN000000001")
                       .a(" ")
                       .bytes();
  ASSERT_EQ(fx.size(), Replaced::kSize);
  const auto m = decode_out<Replaced>(fx);
  EXPECT_EQ(m.token.str(), "TOKEN000000002");
  EXPECT_EQ(m.previous_token.str(), "TOKEN000000001");
  EXPECT_EQ(m.shares, 200u);
  EXPECT_EQ(m.price, 1'501'000u);
  EXPECT_EQ(m.order_ref, 7u);
  EXPECT_EQ(m.order_state, 'L');
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, Canceled) {
  const Bytes fx = Fixture()
                       .a("C")
                       .x(kTimestamp)
                       .a("TOKEN000000001")
                       .x({0x00, 0x00, 0x00, 0x3C})  // 60 shares
                       .a("U")
                       .bytes();
  ASSERT_EQ(fx.size(), Canceled::kSize);
  const auto m = decode_out<Canceled>(fx);
  EXPECT_EQ(m.token.str(), "TOKEN000000001");
  EXPECT_EQ(m.decrement_shares, 60u);
  EXPECT_EQ(m.reason, 'U');
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, Executed) {
  const Bytes fx = Fixture()
                       .a("E")
                       .x(kTimestamp)
                       .a("TOKEN000000001")
                       .x({0x00, 0x00, 0x00, 0x28})  // 40 shares
                       .x({0x00, 0x16, 0xE3, 0x60})  // 150.0000
                       .a("R")
                       .x({0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xE2, 0x40})  // match 123456
                       .bytes();
  ASSERT_EQ(fx.size(), Executed::kSize);
  const auto m = decode_out<Executed>(fx);
  EXPECT_EQ(m.timestamp, kTimestampNs);
  EXPECT_EQ(m.shares, 40u);
  EXPECT_EQ(m.price, 1'500'000u);
  EXPECT_EQ(m.liquidity, 'R');
  EXPECT_EQ(m.match_number, 123456u);
  EXPECT_EQ(encoded(m), fx);
}

TEST(WireFixture, Rejected) {
  const Bytes fx = Fixture().a("J").x(kTimestamp).a("TOKEN000000001").a("S").bytes();
  ASSERT_EQ(fx.size(), Rejected::kSize);
  const auto m = decode_out<Rejected>(fx);
  EXPECT_EQ(m.token.str(), "TOKEN000000001");
  EXPECT_EQ(m.reason, 'S');
  EXPECT_EQ(encoded(m), fx);
}

TEST(Wire, SizesMatchSpecification) {
  EXPECT_EQ(EnterOrder::kSize, 49u);
  EXPECT_EQ(ReplaceOrder::kSize, 47u);
  EXPECT_EQ(CancelOrder::kSize, 19u);
  EXPECT_EQ(SystemEvent::kSize, 10u);
  EXPECT_EQ(Accepted::kSize, 66u);
  EXPECT_EQ(Replaced::kSize, 80u);
  EXPECT_EQ(Canceled::kSize, 28u);
  EXPECT_EQ(Executed::kSize, 40u);
  EXPECT_EQ(Rejected::kSize, 24u);
}

TEST(Wire, RejectsEmptyTruncatedOversizedAndUnknown) {
  InboundMessage in;
  OutboundMessage out;
  EXPECT_EQ(decode_inbound({}, in), DecodeError::Empty);
  EXPECT_EQ(decode_outbound({}, out), DecodeError::Empty);

  Bytes enter = encoded(EnterOrder{});
  enter.pop_back();
  EXPECT_EQ(decode_inbound(enter, in), DecodeError::BadLength);
  enter.push_back(0);
  enter.push_back(0);
  EXPECT_EQ(decode_inbound(enter, in), DecodeError::BadLength);

  Bytes exec = encoded(Executed{});
  exec.resize(exec.size() - 5);
  EXPECT_EQ(decode_outbound(exec, out), DecodeError::BadLength);

  const Bytes unknown = {'?', 1, 2, 3};
  EXPECT_EQ(decode_inbound(unknown, in), DecodeError::UnknownType);
  EXPECT_EQ(decode_outbound(unknown, out), DecodeError::UnknownType);
  // Outbound 'A' is not a valid inbound type and vice versa.
  EXPECT_EQ(decode_inbound(encoded(Accepted{}), in), DecodeError::UnknownType);
  EXPECT_EQ(decode_outbound(encoded(CancelOrder{}), out), DecodeError::UnknownType);
}

TEST(Wire, EncodeRefusesShortBuffer) {
  std::array<std::uint8_t, 10> small{};
  EXPECT_EQ(encode(EnterOrder{}, small), 0u);
  EXPECT_EQ(encode(Replaced{}, small), 0u);
  EXPECT_EQ(encode(SystemEvent{}, small), SystemEvent::kSize);
}

TEST(Wire, ExtremeValuesRoundTrip) {
  Executed e;
  e.timestamp = UINT64_MAX;
  e.token = Token("ZZZZZZZZZZZZZZ");
  e.shares = UINT32_MAX;
  e.price = kMarketPrice;
  e.match_number = UINT64_MAX;
  EXPECT_EQ(decode_out<Executed>(encoded(e)), e);
}

TEST(Wire, AlphaPadsAndTruncates) {
  EXPECT_EQ(Symbol("IBM").raw(), "IBM     ");
  EXPECT_EQ(Symbol("TOOLONGSYMBOL").raw(), "TOOLONGS");
  EXPECT_TRUE(Token().blank());
  EXPECT_EQ(Token("A").str(), "A");
}

TEST(Wire, ValidateEnterOrder) {
  EnterOrder m;
  m.token = Token("T1");
  m.shares = 100;
  m.stock = Symbol("AAPL");
  m.price = 1'500'000;
  EXPECT_EQ(validate(m), 0);

  auto bad = m;
  bad.shares = 0;
  EXPECT_EQ(validate(bad), 'Z');
  bad = m;
  bad.shares = kMaxShares;
  EXPECT_EQ(validate(bad), 'Z');
  bad = m;
  bad.price = 0;
  EXPECT_EQ(validate(bad), 'X');
  bad = m;
  bad.price = kMaxLimitPrice + 1;
  EXPECT_EQ(validate(bad), 'X');
  bad = m;
  bad.price = kMarketPrice;
  EXPECT_EQ(validate(bad), 0);
  bad = m;
  bad.stock = Symbol();
  EXPECT_EQ(validate(bad), 'S');
  bad = m;
  bad.display = '?';
  EXPECT_EQ(validate(bad), 'D');
  bad = m;
  bad.min_qty = 101;
  EXPECT_EQ(validate(bad), 'N');
  bad = m;
  bad.side = 'Q';
  EXPECT_EQ(validate(bad), 'O');
}
