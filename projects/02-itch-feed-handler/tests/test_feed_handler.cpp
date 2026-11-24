#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "itch/encoder.hpp"
#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"
#include "itch/synth.hpp"

using namespace itch;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kLoc = 3;
const Symbol kSym = Symbol::from("TEST");

template <class T>
Bytes wire(const T& m) {
  Bytes b;
  encode(m, b);
  return b;
}

template <class Handler, class T>
ProcessResult feed(Handler& h, const T& m) {
  return h.process(wire(m));
}

Header hdr(Timestamp ts = 1, std::uint16_t locate = kLoc) { return {locate, 0, ts}; }

struct ReplayOutcome {
  std::uint64_t digest = 0;
  std::uint64_t messages = 0;
  std::uint64_t decoded = 0;
  std::uint64_t trades = 0;
  std::uint64_t book_changes = 0;
  FrameStatus end = FrameStatus::End;
};

template <class Index>
ReplayOutcome replay(std::span<const std::uint8_t> file) {
  FeedHandler<Index> h;
  LengthPrefixedReader reader(file);
  ReplayOutcome out;
  std::span<const std::uint8_t> payload;
  for (;;) {
    const FrameStatus fs = reader.next(payload);
    if (fs == FrameStatus::ZeroLength) continue;
    if (fs != FrameStatus::Ok) {
      out.end = fs;
      break;
    }
    const ProcessResult r = h.process(payload);
    out.trades += r.has_trade ? 1 : 0;
    out.book_changes += r.book_locate >= 0 ? 1 : 0;
  }
  out.digest = h.books().digest();
  out.messages = h.counters().messages;
  out.decoded = h.counters().decoded;
  return out;
}

}  // namespace

TEST_CASE("order flow through the full pipeline", "[feed]") {
  FeedHandler<> h;

  ProcessResult r = feed(h, AddOrder{hdr(10), 1, Side::Buy, 1000, kSym, 25'0000});
  CHECK(r.status == DecodeStatus::Ok);
  CHECK(r.book_locate == kLoc);
  CHECK(r.timestamp == 10);
  CHECK_FALSE(r.has_trade);
  CHECK(h.books().book(kLoc)->symbol.view() == "TEST");  // learned from the add

  // Partial execution: a trade at the resting price, seller-initiated.
  r = feed(h, OrderExecuted{hdr(11), 1, 300, 501});
  REQUIRE(r.has_trade);
  CHECK(r.trade.kind == 'E');
  CHECK(r.trade.price == 25'0000);
  CHECK(r.trade.qty == 300);
  CHECK(r.trade.match_number == 501);
  CHECK(r.trade.aggressor == 'S');
  CHECK(r.trade.printable);
  CHECK(r.book_locate == kLoc);

  r = feed(h, OrderCancel{hdr(12), 1, 200});
  CHECK(r.book_locate == kLoc);
  CHECK_FALSE(r.has_trade);

  CHECK(h.books().bbo(kLoc).bid_qty == 500);

  r = feed(h, OrderDelete{hdr(13), 1});
  CHECK(r.book_locate == kLoc);
  CHECK_FALSE(h.books().bbo(kLoc).has_bid);

  const SymbolStats* s = h.stats(kLoc);
  REQUIRE(s != nullptr);
  CHECK(s->trades == 1);
  CHECK(s->volume == 300);
  CHECK(s->sell_volume == 300);
  CHECK(s->buy_volume == 0);
}

TEST_CASE("add with MPID and replace through the pipeline", "[feed]") {
  FeedHandler<> h;
  feed(h, AddOrderMpid{hdr(), 1, Side::Sell, 400, kSym, 30'0000, {'A', 'B', 'C', 'D'}});
  CHECK(h.books().bbo(kLoc).ask_qty == 400);

  const ProcessResult r = feed(h, OrderReplace{hdr(), 1, 2, 250, 29'0000});
  CHECK(r.book_locate == kLoc);
  const Bbo bbo = h.books().bbo(kLoc);
  CHECK(bbo.ask_price == 29'0000);
  CHECK(bbo.ask_qty == 250);
  CHECK_FALSE(h.books().find(1).has_value());
  CHECK(h.books().find(2)->side == Side::Sell);

  // Buyer-initiated execution against the resting sell order.
  const ProcessResult e = feed(h, OrderExecuted{hdr(), 2, 250, 9});
  CHECK(e.trade.aggressor == 'B');
  CHECK(h.stats(kLoc)->buy_volume == 250);
  CHECK_FALSE(h.books().bbo(kLoc).has_ask);
}

TEST_CASE("trade and cross messages never mutate the displayed book", "[feed][semantics]") {
  FeedHandler<> h;
  feed(h, AddOrder{hdr(), 1, Side::Buy, 100, kSym, 10'0000});
  feed(h, AddOrder{hdr(), 2, Side::Sell, 100, kSym, 10'0200});
  const std::uint64_t before = h.books().digest();

  // A non-displayed trade that happens to carry a live order's reference
  // number and price must still leave the book alone.
  ProcessResult r = feed(h, Trade{hdr(), 1, Side::Buy, 100, kSym, 10'0000, 77});
  CHECK(r.book_locate == -1);
  REQUIRE(r.has_trade);
  CHECK(r.trade.kind == 'P');
  CHECK(r.trade.aggressor == ' ');
  CHECK(h.books().digest() == before);

  r = feed(h, CrossTrade{hdr(), 50'000, kSym, 10'0100, 78, 'O'});
  CHECK(r.book_locate == -1);
  REQUIRE(r.has_trade);
  CHECK(r.trade.kind == 'Q');
  CHECK(r.trade.qty == 50'000);
  CHECK(h.books().digest() == before);

  r = feed(h, BrokenTrade{hdr(), 77});
  CHECK(r.book_locate == -1);
  CHECK_FALSE(r.has_trade);
  CHECK(h.books().digest() == before);
  CHECK(h.counters().broken_trades == 1);

  const SymbolStats* s = h.stats(kLoc);
  CHECK(s->volume == 100);
  CHECK(s->hidden_volume == 100);
  CHECK(s->cross_volume == 50'000);
  CHECK(s->buy_volume + s->sell_volume == 0);
}

TEST_CASE("non-printable execution updates the book but not volume", "[feed][semantics]") {
  FeedHandler<> h;
  feed(h, AddOrder{hdr(), 1, Side::Sell, 500, kSym, 40'0000});

  ProcessResult r = feed(h, OrderExecutedWithPrice{hdr(), 1, 200, 1, 'N', 39'9900});
  REQUIRE(r.has_trade);
  CHECK_FALSE(r.trade.printable);
  CHECK(r.trade.price == 39'9900);  // the message's price, not the resting price
  CHECK(h.books().bbo(kLoc).ask_qty == 300);
  CHECK(h.stats(kLoc) == nullptr);  // nothing printable yet

  r = feed(h, OrderExecutedWithPrice{hdr(), 1, 100, 2, 'Y', 39'9900});
  CHECK(r.trade.printable);
  CHECK(h.books().bbo(kLoc).ask_qty == 200);
  CHECK(h.stats(kLoc)->volume == 100);
  CHECK(h.stats(kLoc)->trades == 1);
}

TEST_CASE("administrative messages are decoded and leave the book alone", "[feed]") {
  FeedHandler<> h;
  StockDirectory dir;
  dir.hdr = hdr();
  dir.stock = Symbol::from("DIRNAME");
  CHECK(feed(h, dir).book_locate == -1);
  CHECK(h.books().book(kLoc)->symbol.view() == "DIRNAME");

  // The directory name wins over the symbol carried by later order flow.
  feed(h, AddOrder{hdr(), 1, Side::Buy, 100, kSym, 10'0000});
  CHECK(h.books().book(kLoc)->symbol.view() == "DIRNAME");
  const std::uint64_t before = h.books().digest();

  CHECK(feed(h, SystemEvent{hdr(), 'Q'}).book_locate == -1);
  CHECK(feed(h, StockTradingAction{hdr(), kSym, 'H', ' ', {}}).book_locate == -1);
  CHECK(feed(h, Noii{}).book_locate == -1);
  CHECK(feed(h, Rpii{}).status == DecodeStatus::Ok);
  CHECK(h.books().digest() == before);
  CHECK(h.counters().decoded_by_type['S'] == 1);
  CHECK(h.counters().decoded_by_type['I'] == 1);
}

TEST_CASE("unknown and malformed payloads are counted by policy", "[feed][negative]") {
  FeedHandler<> h;
  feed(h, AddOrder{hdr(), 1, Side::Buy, 100, kSym, 10'0000});
  const std::uint64_t before = h.books().digest();

  const Bytes unknown{'z', 1, 2, 3};
  CHECK(h.process(unknown).status == DecodeStatus::UnknownType);

  Bytes truncated = wire(OrderDelete{hdr(), 1});
  truncated.pop_back();
  CHECK(h.process(truncated).status == DecodeStatus::Truncated);

  Bytes oversized = wire(OrderDelete{hdr(), 1});
  oversized.push_back(0);
  CHECK(h.process(oversized).status == DecodeStatus::Oversized);

  Bytes bad_side = wire(AddOrder{hdr(), 2, Side::Buy, 100, kSym, 10'0000});
  bad_side[19] = '?';
  CHECK(h.process(bad_side).status == DecodeStatus::InvalidField);

  CHECK(h.process({}).status == DecodeStatus::Empty);

  // Well-formed messages about orders that do not exist.
  CHECK(feed(h, OrderExecuted{hdr(), 999, 10, 1}).book_locate == -1);
  CHECK_FALSE(feed(h, OrderExecuted{hdr(), 999, 10, 1}).has_trade);
  CHECK(feed(h, OrderDelete{hdr(), 999}).book_locate == -1);

  // Header locate that disagrees with the order's own locate.
  feed(h, OrderCancel{hdr(1, kLoc + 1), 1, 10});

  const FeedCounters& c = h.counters();
  CHECK(c.unknown_type == 1);
  CHECK(c.truncated == 1);
  CHECK(c.oversized == 1);
  CHECK(c.invalid_field == 1);
  CHECK(c.empty == 1);
  CHECK(c.seen_by_type['z'] == 1);
  CHECK(c.decoded_by_type['z'] == 0);
  CHECK(c.seen_by_type['D'] == 3);
  CHECK(c.decoded_by_type['D'] == 1);
  CHECK(c.locate_mismatch == 1);
  CHECK(h.books().counters().unknown_order == 3);

  // None of the rejected input changed the book; the mismatched cancel did.
  CHECK(h.books().find(1)->qty == 90);
  CHECK(h.books().digest() != before);
  CHECK_FALSE(h.books().find(2).has_value());
}

TEST_CASE("synthetic stream is valid ITCH and byte-identical per seed", "[feed][replay]") {
  const SynthConfig cfg{50'000, 25, 42, SynthProfile::Balanced};
  const Bytes a = generate_synthetic(cfg);
  const Bytes b = generate_synthetic(cfg);
  REQUIRE(a == b);

  SynthConfig other = cfg;
  other.seed = 43;
  CHECK(generate_synthetic(other) != a);

  FeedHandler<> h;
  LengthPrefixedReader reader(a);
  std::span<const std::uint8_t> payload;
  Timestamp last = 0;
  for (;;) {
    const FrameStatus fs = reader.next(payload);
    if (fs != FrameStatus::Ok) {
      REQUIRE(fs == FrameStatus::End);
      break;
    }
    const ProcessResult r = h.process(payload);
    REQUIRE(r.status == DecodeStatus::Ok);
    REQUIRE(r.timestamp >= last);
    last = r.timestamp;
    // The generator keeps bids below asks; a crossed book would mean the
    // builder (or generator) mishandled an order.
    if (r.book_locate >= 0) {
      const Bbo bbo = h.books().bbo(static_cast<std::uint16_t>(r.book_locate));
      if (bbo.has_bid && bbo.has_ask) REQUIRE(bbo.bid_price < bbo.ask_price);
    }
  }
  const FeedCounters& c = h.counters();
  CHECK(c.messages == c.decoded);
  CHECK(c.messages >= cfg.messages);
  // Every supported message type appears in a balanced stream.
  for (const char t : {'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'A', 'F',
                       'E', 'C', 'X', 'D', 'U', 'P', 'Q', 'B', 'I', 'N', 'O'}) {
    INFO("type " << t);
    CHECK(c.decoded_by_type[static_cast<unsigned char>(t)] > 0);
  }
  const BookCounters& bc = h.books().counters();
  CHECK(bc.duplicate_order == 0);
  CHECK(bc.unknown_order == 0);
  CHECK(bc.over_reduce == 0);
  CHECK(c.locate_mismatch == 0);
  CHECK(h.books().live_orders() > 0);
}

TEST_CASE("deterministic replay: repeated runs and both indexes agree", "[feed][replay]") {
  for (const SynthProfile profile :
       {SynthProfile::Balanced, SynthProfile::AddHeavy, SynthProfile::Churn}) {
    INFO("profile " << to_string(profile));
    const Bytes file = generate_synthetic({40'000, 30, 7, profile});
    const ReplayOutcome first = replay<FlatOrderIndex>(file);
    const ReplayOutcome second = replay<FlatOrderIndex>(file);
    const ReplayOutcome other = replay<StdOrderIndex>(file);
    CHECK(first.end == FrameStatus::End);
    CHECK(first.messages == first.decoded);
    CHECK(first.digest == second.digest);
    CHECK(first.trades == second.trades);
    CHECK(first.book_changes == second.book_changes);
    // The index implementation must not affect any observable state.
    CHECK(first.digest == other.digest);
    CHECK(first.trades == other.trades);
    CHECK(first.book_changes == other.book_changes);
  }
}

TEST_CASE("a file cut at any point replays its complete prefix", "[feed][replay][negative]") {
  const Bytes file = generate_synthetic({300, 5, 9, SynthProfile::Balanced});
  const ReplayOutcome whole = replay<FlatOrderIndex>(file);
  std::uint64_t previous = 0;
  for (std::size_t cut = 0; cut <= file.size(); cut += 7) {
    const ReplayOutcome part =
        replay<FlatOrderIndex>(std::span<const std::uint8_t>(file.data(), cut));
    REQUIRE(part.messages == part.decoded);
    REQUIRE(part.messages >= previous);
    REQUIRE(part.messages <= whole.messages);
    previous = part.messages;
  }
}

TEMPLATE_TEST_CASE("fuzz: corrupted streams never crash and counters stay consistent",
                   "[feed][fuzz]", FlatOrderIndex, StdOrderIndex) {
  const Bytes clean = generate_synthetic({4'000, 8, 11, SynthProfile::Balanced});
  std::mt19937_64 rng(99);
  for (int round = 0; round < 60; ++round) {
    Bytes data = clean;
    // Flip bytes anywhere: in length prefixes, type bytes, ids, sizes, prices.
    const std::size_t flips = 1 + rng() % 200;
    for (std::size_t i = 0; i < flips; ++i) {
      data[rng() % data.size()] = static_cast<std::uint8_t>(rng());
    }
    if (rng() % 3 == 0) data.resize(rng() % data.size());

    FeedHandler<TestType> h;
    LengthPrefixedReader reader(data);
    std::span<const std::uint8_t> payload;
    for (;;) {
      const FrameStatus fs = reader.next(payload);
      if (fs == FrameStatus::ZeroLength) continue;
      if (fs != FrameStatus::Ok) break;
      (void)h.process(payload);
    }
    const FeedCounters& c = h.counters();
    REQUIRE(c.messages ==
            c.decoded + c.empty + c.unknown_type + c.truncated + c.oversized + c.invalid_field);
    // Walks every level and queue; under ASan/UBSan this validates the links.
    (void)h.books().digest();
  }
}

TEMPLATE_TEST_CASE("fuzz: arbitrary payload bytes are handled", "[feed][fuzz]", FlatOrderIndex,
                   StdOrderIndex) {
  FeedHandler<TestType> h;
  std::mt19937_64 rng(1234);
  const char types[] = {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'Q'};
  Bytes payload;
  for (int i = 0; i < 300'000; ++i) {
    // Bias toward order-flow types at their correct length so the book is
    // driven hard with adversarial ids, sides, sizes and prices.
    const char type = types[rng() % std::size(types)];
    const std::size_t len = rng() % 8 == 0 ? rng() % 64 : message_length(type);
    payload.resize(len);
    for (auto& byte : payload) byte = static_cast<std::uint8_t>(rng());
    if (len > 0 && rng() % 16 != 0) payload[0] = static_cast<std::uint8_t>(type);
    // Small id space so operations actually hit live orders.
    if (len >= 19) {
      for (std::size_t k = 11; k < 18; ++k) payload[k] = 0;
      payload[18] = static_cast<std::uint8_t>(rng() % 64);
    }
    // Byte 19 starts the new order reference in a replace and is the side
    // in an add/trade, so which fix-up applies depends on the final type byte.
    const char actual = len > 0 ? static_cast<char>(payload[0]) : '\0';
    if (len >= 27 && actual == 'U') {
      for (std::size_t k = 19; k < 26; ++k) payload[k] = 0;
      payload[26] = static_cast<std::uint8_t>(rng() % 64);
    } else if (len >= 20 && rng() % 4 != 0) {
      payload[19] = rng() % 2 == 0 ? 'B' : 'S';
    }
    (void)h.process(payload);
  }
  CHECK(h.counters().messages == 300'000);
  CHECK(h.books().live_orders() <= 64);
  (void)h.books().digest();
}
