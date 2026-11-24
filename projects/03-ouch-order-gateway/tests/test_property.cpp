// Randomised property tests. Seeds are fixed so failures reproduce; these
// complement the libFuzzer targets in tests/fuzz, which need a fuzzing-capable
// clang and are not part of the default build.
#include <gtest/gtest.h>

#include <random>
#include <string>
#include <variant>
#include <vector>

#include "ouch/order_manager.hpp"
#include "ouch/soupbin.hpp"
#include "ouch/wire.hpp"
#include "test_util.hpp"

using namespace ouch;
using namespace ouch::wire;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Rng = std::mt19937_64;

std::uint32_t pick(Rng& rng, std::uint32_t lo, std::uint32_t hi) {
  return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
}

template <std::size_t N>
Alpha<N> random_alpha(Rng& rng) {
  Alpha<N> a;
  for (auto& c : a.c) c = static_cast<char>(pick(rng, 0, 255));
  return a;
}

char random_char(Rng& rng) { return static_cast<char>(pick(rng, 0, 255)); }

InboundMessage random_inbound(Rng& rng) {
  switch (pick(rng, 0, 2)) {
    case 0: {
      EnterOrder m;
      m.token = random_alpha<14>(rng);
      m.side = random_char(rng);
      m.shares = static_cast<std::uint32_t>(rng());
      m.stock = random_alpha<8>(rng);
      m.price = static_cast<std::uint32_t>(rng());
      m.tif = static_cast<std::uint32_t>(rng());
      m.firm = random_alpha<4>(rng);
      m.display = random_char(rng);
      m.capacity = random_char(rng);
      m.iso = random_char(rng);
      m.min_qty = static_cast<std::uint32_t>(rng());
      m.cross_type = random_char(rng);
      m.customer_type = random_char(rng);
      return m;
    }
    case 1: {
      ReplaceOrder m;
      m.existing_token = random_alpha<14>(rng);
      m.replacement_token = random_alpha<14>(rng);
      m.shares = static_cast<std::uint32_t>(rng());
      m.price = static_cast<std::uint32_t>(rng());
      m.tif = static_cast<std::uint32_t>(rng());
      m.display = random_char(rng);
      m.iso = random_char(rng);
      m.min_qty = static_cast<std::uint32_t>(rng());
      return m;
    }
    default:
      return CancelOrder{random_alpha<14>(rng), static_cast<std::uint32_t>(rng())};
  }
}

OutboundMessage random_outbound(Rng& rng) {
  switch (pick(rng, 0, 5)) {
    case 0:
      return SystemEvent{rng(), random_char(rng)};
    case 1: {
      Accepted m;
      m.timestamp = rng();
      m.token = random_alpha<14>(rng);
      m.side = random_char(rng);
      m.shares = static_cast<std::uint32_t>(rng());
      m.stock = random_alpha<8>(rng);
      m.price = static_cast<std::uint32_t>(rng());
      m.tif = static_cast<std::uint32_t>(rng());
      m.firm = random_alpha<4>(rng);
      m.display = random_char(rng);
      m.order_ref = rng();
      m.capacity = random_char(rng);
      m.iso = random_char(rng);
      m.min_qty = static_cast<std::uint32_t>(rng());
      m.cross_type = random_char(rng);
      m.order_state = random_char(rng);
      m.bbo_weight = random_char(rng);
      return m;
    }
    case 2: {
      Replaced m;
      m.timestamp = rng();
      m.token = random_alpha<14>(rng);
      m.side = random_char(rng);
      m.shares = static_cast<std::uint32_t>(rng());
      m.stock = random_alpha<8>(rng);
      m.price = static_cast<std::uint32_t>(rng());
      m.tif = static_cast<std::uint32_t>(rng());
      m.firm = random_alpha<4>(rng);
      m.display = random_char(rng);
      m.order_ref = rng();
      m.capacity = random_char(rng);
      m.iso = random_char(rng);
      m.min_qty = static_cast<std::uint32_t>(rng());
      m.cross_type = random_char(rng);
      m.order_state = random_char(rng);
      m.previous_token = random_alpha<14>(rng);
      m.bbo_weight = random_char(rng);
      return m;
    }
    case 3:
      return Canceled{rng(), random_alpha<14>(rng), static_cast<std::uint32_t>(rng()),
                      random_char(rng)};
    case 4:
      return Executed{rng(),
                      random_alpha<14>(rng),
                      static_cast<std::uint32_t>(rng()),
                      static_cast<std::uint32_t>(rng()),
                      random_char(rng),
                      rng()};
    default:
      return Rejected{rng(), random_alpha<14>(rng), random_char(rng)};
  }
}

}  // namespace

TEST(Property, RandomMessagesRoundTripExactly) {
  Rng rng(0xC0DEC);
  std::array<std::uint8_t, kMaxMessageSize> buf{};
  for (int i = 0; i < 20000; ++i) {
    const InboundMessage in = random_inbound(rng);
    const std::size_t n = encode(in, buf);
    ASSERT_GT(n, 0u);
    InboundMessage in_back;
    ASSERT_EQ(decode_inbound(std::span<const std::uint8_t>(buf.data(), n), in_back),
              DecodeError::None);
    ASSERT_TRUE(in == in_back);

    const OutboundMessage out = random_outbound(rng);
    const std::size_t k = encode(out, buf);
    ASSERT_GT(k, 0u);
    OutboundMessage out_back;
    ASSERT_EQ(decode_outbound(std::span<const std::uint8_t>(buf.data(), k), out_back),
              DecodeError::None);
    ASSERT_TRUE(out == out_back);
  }
}

// Anything the decoder accepts must re-encode to the identical bytes, and
// nothing may read out of bounds (run under ASan/UBSan in CI).
TEST(Property, ArbitraryBytesNeverCrashAndAcceptedInputReencodesIdentically) {
  Rng rng(0xF022);
  const char types[] = {'O', 'U', 'X', 'S', 'A', 'C', 'E', 'J', 'B', 'M', '?'};
  const std::size_t sizes[] = {0, 1, 10, 19, 24, 28, 40, 47, 49, 66, 80, 81};
  std::array<std::uint8_t, kMaxMessageSize> buf{};
  std::size_t accepted_in = 0;
  std::size_t accepted_out = 0;
  for (int i = 0; i < 200000; ++i) {
    std::size_t len = pick(rng, 0, 3) == 0 ? pick(rng, 0, 120) : sizes[pick(rng, 0, 11)];
    Bytes b(len);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    if (!b.empty() && pick(rng, 0, 9) != 0)
      b[0] = static_cast<std::uint8_t>(types[pick(rng, 0, 10)]);

    InboundMessage in;
    if (decode_inbound(b, in) == DecodeError::None) {
      ++accepted_in;
      const std::size_t n = encode(in, buf);
      ASSERT_EQ(n, b.size());
      ASSERT_TRUE(std::equal(b.begin(), b.end(), buf.begin()));
    }
    OutboundMessage out;
    if (decode_outbound(b, out) == DecodeError::None) {
      ++accepted_out;
      const std::size_t n = encode(out, buf);
      ASSERT_EQ(n, b.size());
      ASSERT_TRUE(std::equal(b.begin(), b.end(), buf.begin()));
    }
  }
  // The generator is biased so the accepting paths are really exercised.
  EXPECT_GT(accepted_in, 1000u);
  EXPECT_GT(accepted_out, 1000u);
}

TEST(Property, FrameParserIsInvariantToChunking) {
  Rng rng(0xF4A3E);
  for (int round = 0; round < 200; ++round) {
    Bytes stream;
    std::vector<std::pair<char, Bytes>> expect;
    const std::uint32_t packets = pick(rng, 1, 40);
    for (std::uint32_t i = 0; i < packets; ++i) {
      Bytes payload(pick(rng, 0, 3) == 0 ? pick(rng, 0, 2000) : pick(rng, 0, 80));
      for (auto& x : payload) x = static_cast<std::uint8_t>(rng());
      const char type = static_cast<char>(pick(rng, 0, 255));
      ASSERT_TRUE(soup::append_packet(stream, type, payload));
      expect.emplace_back(type, std::move(payload));
    }

    soup::FrameParser p;
    std::vector<std::pair<char, Bytes>> got;
    std::size_t at = 0;
    while (at < stream.size()) {
      const std::size_t n = std::min<std::size_t>(pick(rng, 1, 300), stream.size() - at);
      p.feed(std::span<const std::uint8_t>(stream.data() + at, n));
      at += n;
      char type = 0;
      std::span<const std::uint8_t> payload;
      while (p.next(type, payload) == soup::FrameParser::Status::Packet) {
        got.emplace_back(type, Bytes(payload.begin(), payload.end()));
      }
    }
    ASSERT_EQ(got, expect);
    ASSERT_EQ(p.buffered(), 0u);
  }
}

TEST(Property, FrameParserSurvivesGarbage) {
  Rng rng(0x6A2BA6E);
  for (int round = 0; round < 2000; ++round) {
    soup::FrameParser p;
    Bytes junk(pick(rng, 0, 600));
    for (auto& x : junk) x = static_cast<std::uint8_t>(rng());
    p.feed(junk);
    char type = 0;
    std::span<const std::uint8_t> payload;
    std::size_t consumed = 0;
    while (p.next(type, payload) == soup::FrameParser::Status::Packet) {
      consumed += payload.size() + 3;
      ASSERT_LE(consumed, junk.size());
    }
  }
}

// Feed the OMS nonsense: random requests and random exchange messages over a
// small pool of tokens, in any order. It may count anomalies, but its books
// must always balance.
TEST(Property, OmsInvariantsHoldUnderArbitraryMessageStorm) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Rng rng(seed);
    OrderManager oms;
    std::vector<Token> tokens;
    for (int i = 0; i < 12; ++i) tokens.emplace_back("T" + std::to_string(i));
    const Symbol syms[] = {Symbol("AAPL"), Symbol("MSFT")};
    const char sides[] = {'B', 'S', 'T'};
    const char reasons[] = {'U', 'U', 'I', 'S', 'Z'};
    std::uint32_t next_replacement = 0;
    const auto tok = [&] {
      return tokens[pick(rng, 0, static_cast<std::uint32_t>(tokens.size()) - 1)];
    };
    const auto qty = [&] { return pick(rng, 0, 4) * 50; };

    for (int step = 0; step < 4000; ++step) {
      switch (pick(rng, 0, 9)) {
        case 0: {
          EnterOrder e;
          e.token = tok();
          e.side = sides[pick(rng, 0, 2)];
          e.shares = qty() + 50;
          e.stock = syms[pick(rng, 0, 1)];
          e.price = 1'000'000 + pick(rng, 0, 10) * 100;
          oms.on_enter_sent(e, static_cast<std::uint64_t>(step));
          break;
        }
        case 1:
          oms.on_cancel_sent(CancelOrder{tok(), qty()}, static_cast<std::uint64_t>(step));
          break;
        case 2: {
          const Token fresh("R" + std::to_string(next_replacement++));
          if (oms.on_replace_sent(
                  ReplaceOrder{tok(), fresh, qty() + 50, 1'000'000, kTifMarketHours, 'Y', 'N', 0},
                  static_cast<std::uint64_t>(step))) {
            tokens.push_back(fresh);
          }
          break;
        }
        case 3: {
          Accepted a;
          a.token = tok();
          a.shares = qty();
          a.order_ref = pick(rng, 0, 3);
          a.order_state = pick(rng, 0, 9) == 0 ? 'D' : 'L';
          oms.on_exchange(a, static_cast<std::uint64_t>(step));
          break;
        }
        case 4:
        case 5:
          oms.on_exchange(Executed{0, tok(), qty(), 1'000'000, 'A', pick(rng, 0, 400)},
                          static_cast<std::uint64_t>(step));
          break;
        case 6:
          oms.on_exchange(Canceled{0, tok(), qty(), reasons[pick(rng, 0, 4)]},
                          static_cast<std::uint64_t>(step));
          break;
        case 7:
          oms.on_exchange(Rejected{0, tok(), 'O'}, static_cast<std::uint64_t>(step));
          break;
        case 8: {
          Replaced r;
          r.token = tok();
          r.previous_token = tok();
          r.shares = qty();
          r.order_ref = pick(rng, 0, 3);
          oms.on_exchange(r, static_cast<std::uint64_t>(step));
          break;
        }
        default:
          if (pick(rng, 0, 3) == 0) {
            oms.on_disconnect();
          } else {
            oms.on_resent(tok());
          }
          break;
      }
      const std::string problem = oms.check_invariants();
      ASSERT_EQ(problem, "") << "seed " << seed << " step " << step;
    }
  }
}

// A coherent exchange stream, delivered with every message repeated a random
// number of times and then replayed in full, must leave exactly the state
// that a single clean delivery leaves.
TEST(Property, OmsIsIdempotentUnderDuplicationAndFullReplay) {
  using Step = std::variant<InboundMessage, OutboundMessage>;
  for (std::uint64_t seed = 1; seed <= 30; ++seed) {
    Rng rng(seed * 7919);
    struct Model {
      Token token;
      char side;
      std::uint32_t open;
      bool live;
      bool cancel_requested;
    };
    std::vector<Model> book;
    std::vector<Step> script;
    std::uint64_t match = 1;
    std::uint64_t ref = 1;

    for (int step = 0; step < 600; ++step) {
      const std::uint32_t action = pick(rng, 0, 9);
      if (action < 3 || book.empty()) {
        EnterOrder e;
        e.token = Token("M" + std::to_string(book.size()));
        e.side = pick(rng, 0, 1) == 0 ? 'B' : 'S';
        e.shares = pick(rng, 1, 5) * 100;
        e.stock = Symbol("AAPL");
        e.price = 1'500'000;
        script.emplace_back(InboundMessage(e));
        Accepted a;
        a.token = e.token;
        a.side = e.side;
        a.shares = e.shares;
        a.stock = e.stock;
        a.price = e.price;
        a.order_ref = ref++;
        script.emplace_back(OutboundMessage(a));
        book.push_back(Model{e.token, e.side, e.shares, true, false});
        continue;
      }
      Model& m = book[pick(rng, 0, static_cast<std::uint32_t>(book.size()) - 1)];
      if (!m.live) continue;
      if (action < 7) {
        const std::uint32_t fill = std::min(m.open, pick(rng, 1, 3) * 50);
        script.emplace_back(OutboundMessage(Executed{0, m.token, fill, 1'500'000, 'A', match++}));
        m.open -= fill;
      } else if (action == 7) {
        // Exchange-initiated cancel of everything left.
        script.emplace_back(OutboundMessage(Canceled{0, m.token, m.open, 'S'}));
        m.open = 0;
      } else if (!m.cancel_requested) {
        // User cancel down to a random target (possibly a partial cancel).
        const std::uint32_t target = pick(rng, 0, 1) == 0 ? 0 : m.open / 2;
        if (target >= m.open) continue;
        m.cancel_requested = true;
        script.emplace_back(InboundMessage(CancelOrder{m.token, target}));
        script.emplace_back(OutboundMessage(Canceled{0, m.token, m.open - target, 'U'}));
        m.open = target;
      }
      if (m.open == 0) m.live = false;
    }

    OrderManager clean;
    OrderManager noisy;
    for (const Step& s : script) {
      if (const auto* in = std::get_if<InboundMessage>(&s)) {
        ASSERT_TRUE(clean.on_request_sent(*in, 1));
        ASSERT_TRUE(noisy.on_request_sent(*in, 1));
      } else {
        const auto& out = std::get<OutboundMessage>(s);
        clean.on_exchange(out, 2);
        for (std::uint32_t k = pick(rng, 1, 3); k > 0; --k) noisy.on_exchange(out, 2);
      }
    }
    for (const Step& s : script) {
      if (const auto* out = std::get_if<OutboundMessage>(&s)) noisy.on_exchange(*out, 3);
    }

    ASSERT_EQ(clean.check_invariants(), "") << "seed " << seed;
    ASSERT_EQ(noisy.check_invariants(), "") << "seed " << seed;
    ASSERT_EQ(testutil::diff(clean, noisy), "") << "seed " << seed;
    ASSERT_EQ(clean.exposure(Symbol("AAPL")), noisy.exposure(Symbol("AAPL"))) << "seed " << seed;
    ASSERT_EQ(clean.stats().anomalies, 0u);
    ASSERT_EQ(noisy.stats().anomalies, 0u);
    ASSERT_GT(noisy.stats().duplicates, 0u);

    // The model's idea of open shares matches the OMS.
    for (const Model& m : book) ASSERT_EQ(clean.find(m.token)->leaves, m.open);
  }
}
