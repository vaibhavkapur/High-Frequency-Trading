#include <limits>
#include <type_traits>

#include "hft/adapters/backtester_adapter.hpp"
#include "hft/adapters/itch_adapter.hpp"
#include "hft/adapters/ouch_adapter.hpp"
#include "hft/contracts/v1/events.hpp"
#include "hft/contracts/v1/types.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;

// Compile-time invariants: SimTimeNs and WallTimeNs cannot implicitly mix;
// FeedOrderRef and ClientToken cannot implicitly mix.
static_assert(!std::is_convertible_v<SimTimeNs, WallTimeNs>);
static_assert(!std::is_convertible_v<WallTimeNs, SimTimeNs>);
static_assert(!std::is_convertible_v<FeedOrderRef, ClientToken>);
static_assert(!std::is_convertible_v<ClientToken, FeedOrderRef>);

HFT_TEST(FixedPointTickLotAndOverflowValidation) {
  const auto inst = InstrumentMeta::make(0, "AAPL", 100, 10);  // $0.01 tick, 10-share lot
  // $150.25 -> 15025 ticks -> 1,502,500 Price4
  const auto p4 = inst.price_to_price4(Price{15025});
  HFT_EXPECT_TRUE(p4.has_value());
  HFT_EXPECT_EQ(*p4, 1'502'500U);

  const auto back = inst.price4_to_price(*p4);
  HFT_EXPECT_TRUE(back.has_value());
  HFT_EXPECT_EQ(back->ticks, 15025);

  // Unaligned Price4 ($150.255 = 1,502,550 -> not a multiple of 100) must reject!
  HFT_EXPECT_TRUE(!inst.price4_to_price(1'502'550U).has_value());
  // Zero/negative price rejected.
  HFT_EXPECT_TRUE(!inst.price_to_price4(Price{0}).has_value());
  HFT_EXPECT_TRUE(!inst.price_to_price4(Price{-10}).has_value());
  // Overflowing price rejected.
  HFT_EXPECT_TRUE(!inst.price_to_price4(Price{std::numeric_limits<std::int64_t>::max()}).has_value());

  // Lot conversion & alignment check.
  const auto sh = inst.qty_to_shares(Quantity{7});
  HFT_EXPECT_TRUE(sh.has_value());
  HFT_EXPECT_EQ(*sh, 70ULL);
  HFT_EXPECT_TRUE(!inst.shares_to_qty(75ULL).has_value());  // not divisible by lot_size=10
  HFT_EXPECT_TRUE(inst.shares_to_qty(70ULL).has_value());

  // Exact fixed-point notional & fee calculation (no double drift).
  // 15025 ticks ($150.25) * 7 lots (70 shares) = $10,517.50 = 105,175,000 scaled ($0.0001).
  const auto notional = inst.notional_scaled(Price{15025}, Quantity{7});
  HFT_EXPECT_TRUE(notional.has_value());
  HFT_EXPECT_EQ(notional->scaled, 105'175'000LL);

  // Taker fee +30 bps_x100 (+0.30 bps = 3e-5): 105,175,000 * 30 / 1,000,000 = 3155.25 -> 3155
  const auto fee = InstrumentMeta::compute_fee(*notional, 30);
  HFT_EXPECT_TRUE(fee.has_value());
  HFT_EXPECT_EQ(fee->scaled, 3155LL);

  // Maker rebate -20 bps_x100 (-0.20 bps): -2103.5 -> -2104 (half away from zero)
  const auto rebate = InstrumentMeta::compute_fee(*notional, -20);
  HFT_EXPECT_TRUE(rebate.has_value());
  HFT_EXPECT_EQ(rebate->scaled, -2104LL);
}

HFT_TEST(EnvelopeRoundTripAndVersionMismatch) {
  Envelope orig{};
  orig.sequence = 99;
  orig.event_time_ns = SimTimeNs{123456789LL};
  orig.priority = PriorityClass::GatewayExecDelivery;
  orig.instrument = 2;
  orig.venue = 1;

  ExecutionReport er{};
  er.type = ExecReportType::PartiallyFilled;
  er.client_order_id = 42;
  er.token = ClientToken::from_uint64("ORD", 42);
  er.exchange_order_id = 777;
  er.exec_id = 9001;
  er.side = Side::Sell;
  er.exec_price = Price{10'050};
  er.requested_qty = Quantity{100};
  er.fill_qty = Quantity{40};
  er.cum_filled_qty = Quantity{40};
  er.leaves_qty = Quantity{60};
  er.fee_scaled = Money{-120};
  er.is_maker = true;
  er.exchange_time_ns = SimTimeNs{123450000LL};
  er.gateway_recv_time_ns = SimTimeNs{123456789LL};
  orig.payload = er;

  auto bytes = encode_envelope(orig);
  Envelope decoded{};
  HFT_EXPECT_EQ(decode_envelope(bytes, decoded), ErrorCode::Ok);
  HFT_EXPECT_TRUE(decoded == orig);

  // Mutate major version -> must fail with VersionMismatch!
  bytes[6] = 2;  // major version low byte = 2
  Envelope bad{};
  HFT_EXPECT_EQ(decode_envelope(bytes, bad), ErrorCode::VersionMismatch);

  // Truncated buffer -> DecodeError.
  bytes[6] = 1;
  HFT_EXPECT_EQ(decode_envelope(std::span<const std::uint8_t>(bytes.data(), 10), bad),
                ErrorCode::DecodeError);
}

HFT_TEST(ItchAndOuchProtocolBoundaryIsolation) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "S00001", 100, 1)};

  // 1. ITCH 5.0 stream decodes exclusively into MarketDataEvent with FeedOrderRef.
  const auto itch_bytes = ItchFeedAdapter::generate_synthetic_stream(300, 1, 7);
  ItchFeedAdapter itch_ad(0, insts);
  std::vector<Envelope> md_events;
  HFT_EXPECT_EQ(itch_ad.decode_stream(itch_bytes, md_events), ErrorCode::Ok);
  HFT_EXPECT_TRUE(!md_events.empty());
  for (const auto& env : md_events) {
    HFT_EXPECT_TRUE(std::holds_alternative<MarketDataEvent>(env.payload));
    const auto& md = std::get<MarketDataEvent>(env.payload);
    HFT_EXPECT_TRUE(md.feed_order_ref.value > 0);
  }

  // 2. OUCH inbound/outbound round-trip through Project 03 wire codec.
  OuchProtocolAdapter ouch_ad(insts);
  OrderIntent oi{};
  oi.action = OrderAction::New;
  oi.client_order_id = 55;
  oi.token = ClientToken::from_uint64("GW", 55);
  oi.side = Side::Buy;
  oi.order_type = OrderType::Limit;
  oi.tif = TimeInForce::GTC;
  oi.limit_price = Price{10'025};
  oi.qty = Quantity{50};

  ouch::wire::InboundMessage in_msg;
  std::vector<std::uint8_t> wire;
  HFT_EXPECT_EQ(ouch_ad.to_ouch_inbound(0, oi, in_msg, &wire), ErrorCode::Ok);
  HFT_EXPECT_TRUE(!wire.empty());

  ouch::wire::InboundMessage decoded_in;
  HFT_EXPECT_EQ(ouch::wire::decode_inbound(wire, decoded_in), ouch::wire::DecodeError::None);
  const auto* eo = std::get_if<ouch::wire::EnterOrder>(&decoded_in);
  HFT_EXPECT_TRUE(eo != nullptr);
  HFT_EXPECT_EQ(eo->price, 1'002'500U);
  HFT_EXPECT_EQ(eo->shares, 50U);
  HFT_EXPECT_EQ(OuchProtocolAdapter::from_ouch_token(eo->token), oi.token);

  // 3. Causal latency adapter from Project 04 maintains monotonic FIFO delivery.
  CausalLatencyAdapter lat(50'000, 10'000, 80'000, 10'000, 60'000, 10'000, 42);
  SimTimeNs prev{0};
  for (std::int64_t t = 1'000'000; t <= 1'020'000; t += 1'000) {
    const SimTimeNs d = lat.deliver_md(SimTimeNs{t});
    HFT_EXPECT_TRUE(d.value >= t + 50'000);
    HFT_EXPECT_TRUE(d >= prev);
    prev = d;
  }
}

int main() { return hft::test::run_all("test_contracts"); }
