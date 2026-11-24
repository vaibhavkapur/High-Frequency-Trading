#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "itch/decoder.hpp"
#include "itch/encoder.hpp"
#include "itch/messages.hpp"

using namespace itch;

namespace {

using Bytes = std::vector<std::uint8_t>;

template <class T>
T decode_as(const Bytes& b) {
  Message m;
  REQUIRE(decode_message(b, m) == DecodeStatus::Ok);
  REQUIRE(std::holds_alternative<T>(m));
  return std::get<T>(m);
}

// Copies into an exact-size heap block so AddressSanitizer flags any read
// past the end of the payload.
DecodeStatus decode_exact(const std::uint8_t* data, std::size_t n) {
  const auto heap = std::make_unique<std::uint8_t[]>(n == 0 ? 1 : n);
  for (std::size_t i = 0; i < n; ++i) heap[i] = data[i];
  Message m;
  return decode_message(std::span<const std::uint8_t>(heap.get(), n), m);
}

constexpr char kAllTypes[] = {'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'A', 'F',
                              'E', 'C', 'X', 'D', 'U', 'P', 'Q', 'B', 'I', 'N', 'O'};

}  // namespace

// ---- hand-authored byte fixtures: every byte below was written from the
// ---- specification's field table, not produced by the encoder.

// clang-format off: byte tables are laid out one field per line.
TEST_CASE("Add Order (A) fixture decodes field by field", "[decoder][fixture]") {
  const Bytes b{
      'A',                                             // 0  message type
      0x00, 0x2A,                                      // 1  stock locate = 42
      0x00, 0x07,                                      // 3  tracking number = 7
      0x1F, 0x1A, 0x9B, 0xE5, 0x5E, 0x00,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39,  // 11 order ref = 12345
      'B',                                             // 19 side
      0x00, 0x00, 0x01, 0xF4,                          // 20 shares = 500
      'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',   // 24 stock
      0x00, 0x1B, 0x7C, 0x74,                          // 32 price = 180.1332
  };
  REQUIRE(b.size() == AddOrder::kLength);
  const auto m = decode_as<AddOrder>(b);
  CHECK(m.hdr.stock_locate == 42);
  CHECK(m.hdr.tracking_number == 7);
  CHECK(m.hdr.timestamp == 0x1F1A9BE55E00ull);
  CHECK(m.order_ref == 12345);
  CHECK(m.side == Side::Buy);
  CHECK(m.shares == 500);
  CHECK(m.stock.view() == "AAPL");
  CHECK(m.price == 1801332);
}


TEST_CASE("Add Order with MPID (F) fixture", "[decoder][fixture]") {
  const Bytes b{
      'F',                                             // 0  message type
      0x00, 0x01,                                      // 1  stock locate = 1
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x64,              // 5  timestamp = 100
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,  // 11 order ref = 9
      'S',                                             // 19 side
      0x00, 0x00, 0x00, 0x64,                          // 20 shares = 100
      'M',  'S',  'F',  'T',  ' ',  ' ',  ' ',  ' ',   // 24 stock
      0x00, 0x00, 0x27, 0x10,                          // 32 price = 1.0000
      'N',  'I',  'T',  'E',                           // 36 attribution
  };
  REQUIRE(b.size() == AddOrderMpid::kLength);
  const auto m = decode_as<AddOrderMpid>(b);
  CHECK(m.hdr.timestamp == 100);
  CHECK(m.order_ref == 9);
  CHECK(m.side == Side::Sell);
  CHECK(m.shares == 100);
  CHECK(m.stock.view() == "MSFT");
  CHECK(m.price == 10000);
  CHECK(m.attribution == std::array<char, 4>{'N', 'I', 'T', 'E'});
}

TEST_CASE("Order Executed (E) fixture", "[decoder][fixture]") {
  const Bytes b{
      'E',                                             // 0  message type
      0x00, 0x02,                                      // 1  stock locate = 2
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // 11 order ref
      0x00, 0x00, 0x00, 0x32,                          // 19 executed shares = 50
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,  // 23 match number = 1000
  };
  REQUIRE(b.size() == OrderExecuted::kLength);
  const auto m = decode_as<OrderExecuted>(b);
  CHECK(m.hdr.stock_locate == 2);
  CHECK(m.order_ref == 0x0102030405060708ull);
  CHECK(m.executed_shares == 50);
  CHECK(m.match_number == 1000);
}

TEST_CASE("Order Executed With Price (C) fixture", "[decoder][fixture]") {
  const Bytes b{
      'C',                                             // 0  message type
      0x00, 0x02,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,  // 11 order ref = 5
      0x00, 0x00, 0x00, 0x0A,                          // 19 executed shares = 10
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,  // 23 match number = 2
      'N',                                             // 31 printable
      0x00, 0x01, 0x86, 0xA0,                          // 32 execution price = 10.0000
  };
  REQUIRE(b.size() == OrderExecutedWithPrice::kLength);
  const auto m = decode_as<OrderExecutedWithPrice>(b);
  CHECK(m.order_ref == 5);
  CHECK(m.executed_shares == 10);
  CHECK(m.match_number == 2);
  CHECK(m.printable == 'N');
  CHECK(m.execution_price == 100000);
}

TEST_CASE("Order Cancel (X) and Delete (D) fixtures", "[decoder][fixture]") {
  const Bytes x{
      'X',                                             // 0  message type
      0x00, 0x03,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,  // 11 order ref = 6
      0x00, 0x00, 0x00, 0x19,                          // 19 cancelled shares = 25
  };
  REQUIRE(x.size() == OrderCancel::kLength);
  const auto cancel = decode_as<OrderCancel>(x);
  CHECK(cancel.order_ref == 6);
  CHECK(cancel.cancelled_shares == 25);

  const Bytes d{
      'D',                                             // 0  message type
      0x00, 0x03,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,  // 11 order ref = 6
  };
  REQUIRE(d.size() == OrderDelete::kLength);
  CHECK(decode_as<OrderDelete>(d).order_ref == 6);
}

TEST_CASE("Order Replace (U) fixture", "[decoder][fixture]") {
  const Bytes b{
      'U',                                             // 0  message type
      0x00, 0x04,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A,  // 11 original order ref = 10
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B,  // 19 new order ref = 11
      0x00, 0x00, 0x00, 0xC8,                          // 27 shares = 200
      0x00, 0x00, 0x4E, 0x20,                          // 31 price = 2.0000
  };
  REQUIRE(b.size() == OrderReplace::kLength);
  const auto m = decode_as<OrderReplace>(b);
  CHECK(m.original_order_ref == 10);
  CHECK(m.new_order_ref == 11);
  CHECK(m.shares == 200);
  CHECK(m.price == 20000);
}

TEST_CASE("Trade (P) and Cross Trade (Q) fixtures", "[decoder][fixture]") {
  const Bytes p{
      'P',                                             // 0  message type
      0x00, 0x05,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 11 order ref = 0
      'B',                                             // 19 side
      0x00, 0x00, 0x00, 0x64,                          // 20 shares = 100
      'T',  'S',  'L',  'A',  ' ',  ' ',  ' ',  ' ',   // 24 stock
      0x00, 0x03, 0x0D, 0x40,                          // 32 price = 20.0000
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // 36 match number = 7
  };
  REQUIRE(p.size() == Trade::kLength);
  const auto trade = decode_as<Trade>(p);
  CHECK(trade.shares == 100);
  CHECK(trade.stock.view() == "TSLA");
  CHECK(trade.price == 200000);
  CHECK(trade.match_number == 7);

  const Bytes q{
      'Q',                                             // 0  message type
      0x00, 0x05,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,  // 11 shares = 2^32 (needs 64 bits)
      'T',  'S',  'L',  'A',  ' ',  ' ',  ' ',  ' ',   // 19 stock
      0x00, 0x03, 0x0D, 0x40,                          // 27 cross price = 20.0000
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,  // 31 match number = 8
      'O',                                             // 39 cross type
  };
  REQUIRE(q.size() == CrossTrade::kLength);
  const auto cross = decode_as<CrossTrade>(q);
  CHECK(cross.shares == 0x100000000ull);
  CHECK(cross.cross_price == 200000);
  CHECK(cross.match_number == 8);
  CHECK(cross.cross_type == 'O');
}

TEST_CASE("Stock Directory (R) fixture", "[decoder][fixture]") {
  const Bytes b{
      'R',                                             // 0  message type
      0x00, 0x01,                                      // 1  stock locate
      0x00, 0x00,                                      // 3  tracking number
      0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // 5  timestamp
      'Z',  'V',  'Z',  'Z',  'T',  ' ',  ' ',  ' ',   // 11 stock
      'Q',                                             // 19 market category
      'N',                                             // 20 financial status
      0x00, 0x00, 0x00, 0x64,                          // 21 round lot size = 100
      'N',                                             // 25 round lots only
      'C',                                             // 26 issue classification
      'Z',  ' ',                                       // 27 issue sub-type
      'P',                                             // 29 authenticity
      'N',                                             // 30 short sale threshold
      'N',                                             // 31 IPO flag
      '2',                                             // 32 LULD reference price tier
      'Y',                                             // 33 ETP flag
      0x00, 0x00, 0x00, 0x03,                          // 34 ETP leverage factor = 3
      'Y',                                             // 38 inverse indicator
  };
  REQUIRE(b.size() == StockDirectory::kLength);
  const auto m = decode_as<StockDirectory>(b);
  CHECK(m.stock.view() == "ZVZZT");
  CHECK(m.market_category == 'Q');
  CHECK(m.round_lot_size == 100);
  CHECK(m.issue_subtype == std::array<char, 2>{'Z', ' '});
  CHECK(m.luld_tier == '2');
  CHECK(m.etp_flag == 'Y');
  CHECK(m.etp_leverage == 3);
  CHECK(m.inverse_indicator == 'Y');
}

// clang-format on

// ---- properties over the whole message matrix -------------------------------

TEST_CASE("specification lengths", "[decoder]") {
  // Message lengths from the ITCH 5.0 specification, restated independently
  // of the struct definitions.
  const std::pair<char, std::size_t> expected[] = {
      {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
      {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
      {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48},
  };
  for (const auto& [type, len] : expected) {
    INFO("type " << type);
    CHECK(message_length(type) == len);
  }
  CHECK(message_length('Z') == 0);
  CHECK(std::size(expected) == std::size(kAllTypes));
}

TEST_CASE("decode then encode reproduces the wire bytes for every type", "[decoder][property]") {
  std::mt19937_64 rng(20240601);
  for (const char type : kAllTypes) {
    INFO("type " << type);
    const std::size_t len = message_length(type);
    REQUIRE(len != 0);
    int accepted = 0;
    for (int iter = 0; iter < 400; ++iter) {
      Bytes wire(len);
      for (auto& byte : wire) byte = static_cast<std::uint8_t>(rng());
      wire[0] = static_cast<std::uint8_t>(type);
      Message m;
      const DecodeStatus st = decode_exact(wire.data(), wire.size());
      if (st == DecodeStatus::InvalidField) continue;  // random side byte; covered below
      REQUIRE(st == DecodeStatus::Ok);
      REQUIRE(decode_message(wire, m) == DecodeStatus::Ok);
      Bytes again;
      encode(m, again);
      REQUIRE(again == wire);
      ++accepted;
    }
    // Types with a side field accept ~2/256 of random inputs; force validity
    // so they are exercised too.
    if (type == 'A' || type == 'F' || type == 'P') {
      for (int iter = 0; iter < 400; ++iter) {
        Bytes wire(len);
        for (auto& byte : wire) byte = static_cast<std::uint8_t>(rng());
        wire[0] = static_cast<std::uint8_t>(type);
        wire[19] = (rng() & 1) != 0 ? 'B' : 'S';
        Message m;
        REQUIRE(decode_message(wire, m) == DecodeStatus::Ok);
        Bytes again;
        encode(m, again);
        REQUIRE(again == wire);
        ++accepted;
      }
    }
    CHECK(accepted >= 400);
  }
}

// ---- malformed input --------------------------------------------------------

TEST_CASE("every truncation of every type is rejected without over-reading",
          "[decoder][negative]") {
  for (const char type : kAllTypes) {
    INFO("type " << type);
    const std::size_t len = message_length(type);
    Bytes wire(len, 0x41);
    wire[0] = static_cast<std::uint8_t>(type);
    if (type == 'A' || type == 'F' || type == 'P') wire[19] = 'B';
    REQUIRE(decode_exact(wire.data(), len) == DecodeStatus::Ok);
    for (std::size_t n = 1; n < len; ++n) {
      REQUIRE(decode_exact(wire.data(), n) == DecodeStatus::Truncated);
    }
  }
}

TEST_CASE("oversized, empty and unknown payloads", "[decoder][negative]") {
  Bytes wire(OrderDelete::kLength + 1, 0);
  wire[0] = 'D';
  CHECK(decode_exact(wire.data(), wire.size()) == DecodeStatus::Oversized);
  CHECK(decode_exact(wire.data(), 0) == DecodeStatus::Empty);

  // Every byte value that is not a supported type is UnknownType at any length.
  for (int t = 0; t < 256; ++t) {
    if (message_length(static_cast<char>(t)) != 0) continue;
    Bytes unknown(40, 0);
    unknown[0] = static_cast<std::uint8_t>(t);
    REQUIRE(decode_exact(unknown.data(), unknown.size()) == DecodeStatus::UnknownType);
    REQUIRE(decode_exact(unknown.data(), 1) == DecodeStatus::UnknownType);
  }
}

TEST_CASE("invalid side is rejected and the handler is not called", "[decoder][negative]") {
  std::vector<std::uint8_t> wire;
  encode(AddOrder{{1, 0, 5}, 77, Side::Buy, 100, Symbol::from("X"), 10000}, wire);
  wire[19] = 'Q';
  int calls = 0;
  const DecodeStatus st = decode(wire, [&calls](const auto&) { ++calls; });
  CHECK(st == DecodeStatus::InvalidField);
  CHECK(calls == 0);
}

TEST_CASE("symbol view trims padding", "[messages]") {
  CHECK(Symbol::from("AB").view() == "AB");
  CHECK(Symbol::from("ABCDEFGH").view() == "ABCDEFGH");
  CHECK(Symbol::from("ABCDEFGHIJ").view() == "ABCDEFGH");
  CHECK(Symbol{}.view().empty());
}
