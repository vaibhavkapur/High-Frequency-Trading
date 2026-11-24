#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ouch/soupbin.hpp"

using namespace ouch;
using namespace ouch::soup;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes ascii(std::string_view s) { return {s.begin(), s.end()}; }

struct Packet {
  char type;
  Bytes payload;
  friend bool operator==(const Packet&, const Packet&) = default;
};

std::vector<Packet> drain(FrameParser& p, FrameParser::Status* last = nullptr) {
  std::vector<Packet> out;
  char type = 0;
  std::span<const std::uint8_t> payload;
  FrameParser::Status st;
  while ((st = p.next(type, payload)) == FrameParser::Status::Packet) {
    out.push_back(Packet{type, Bytes(payload.begin(), payload.end())});
  }
  if (last != nullptr) *last = st;
  return out;
}

}  // namespace

TEST(SoupBin, PacketFraming) {
  Bytes out;
  ASSERT_TRUE(append_packet(out, kUnsequencedData, ascii("abc")));
  EXPECT_EQ(out, (Bytes{0x00, 0x04, 'U', 'a', 'b', 'c'}));

  out.clear();
  ASSERT_TRUE(append_packet(out, kServerHeartbeat));
  EXPECT_EQ(out, (Bytes{0x00, 0x01, 'H'}));
}

TEST(SoupBin, RefusesOversizedPayload) {
  Bytes out;
  const Bytes big(0xFFFF, 0);
  EXPECT_FALSE(append_packet(out, kSequencedData, big));
  EXPECT_TRUE(out.empty());
  const Bytes max(0xFFFE, 0);
  EXPECT_TRUE(append_packet(out, kSequencedData, max));
  EXPECT_EQ(out.size(), 0xFFFEu + 3);
}

TEST(SoupBin, LoginRequestFixture) {
  LoginRequest m;
  m.username = Username("user");
  m.password = Password("secret");
  m.session = SessionId();
  m.seq = 42;
  Bytes out;
  append_login_request(out, m);

  // length 47 = type + 46 payload bytes
  Bytes expect = {0x00, 0x2F, 'L'};
  const Bytes body = ascii(
      "user  "                  // username: left-justified, 6
      "secret    "              // password: left-justified, 10
      "          "              // session: blank, 10
      "                  42");  // sequence: right-justified, 20
  expect.insert(expect.end(), body.begin(), body.end());
  EXPECT_EQ(out, expect);

  LoginRequest back;
  ASSERT_TRUE(parse_login_request(std::span<const std::uint8_t>(out).subspan(3), back));
  EXPECT_EQ(back, m);
}

TEST(SoupBin, LoginAcceptedFixture) {
  Bytes out;
  append_login_accepted(out, LoginAccepted{SessionId("SIM0000001"), 1});
  Bytes expect = {0x00, 0x1F, 'A'};
  const Bytes body = ascii(
      "SIM0000001"
      "                   1");
  expect.insert(expect.end(), body.begin(), body.end());
  EXPECT_EQ(out, expect);

  LoginAccepted back;
  ASSERT_TRUE(parse_login_accepted(std::span<const std::uint8_t>(out).subspan(3), back));
  EXPECT_EQ(back.session.str(), "SIM0000001");
  EXPECT_EQ(back.seq, 1u);
}

TEST(SoupBin, LoginParsersRejectBadInput) {
  // Session id followed by a 20-character sequence field.
  const auto with_seq = [](const std::string& field) {
    EXPECT_EQ(field.size(), 20u);
    return ascii("SIM0000001" + field);
  };
  LoginAccepted a;
  EXPECT_FALSE(parse_login_accepted(ascii("short"), a));
  EXPECT_FALSE(parse_login_accepted(with_seq(std::string(20, ' ')), a));          // blank
  EXPECT_FALSE(parse_login_accepted(with_seq(std::string(17, ' ') + "x12"), a));  // not digits
  EXPECT_FALSE(parse_login_accepted(with_seq("12" + std::string(18, ' ')), a));   // left-justified
  EXPECT_FALSE(parse_login_accepted(with_seq(std::string(20, '9')), a));          // overflow
  EXPECT_TRUE(parse_login_accepted(with_seq("18446744073709551615"), a));
  EXPECT_EQ(a.seq, UINT64_MAX);
  LoginRequest r;
  EXPECT_FALSE(parse_login_request(ascii("nope"), r));
}

TEST(SoupBin, SequenceNumberRoundTripsLargeValues) {
  for (std::uint64_t seq : {0ULL, 1ULL, 9ULL, 10ULL, 123456789012345ULL, UINT64_MAX}) {
    Bytes out;
    append_login_accepted(out, LoginAccepted{SessionId("S"), seq});
    LoginAccepted back;
    ASSERT_TRUE(parse_login_accepted(std::span<const std::uint8_t>(out).subspan(3), back));
    EXPECT_EQ(back.seq, seq);
  }
}

TEST(FrameParser, ReassemblesByteByByte) {
  Bytes stream;
  append_packet(stream, kSequencedData, ascii("hello"));
  append_packet(stream, kServerHeartbeat);
  append_packet(stream, kUnsequencedData, ascii("x"));

  FrameParser p;
  std::vector<Packet> got;
  for (std::uint8_t b : stream) {
    p.feed(std::span<const std::uint8_t>(&b, 1));
    for (auto& pkt : drain(p)) got.push_back(std::move(pkt));
  }
  const std::vector<Packet> expect = {
      {kSequencedData, ascii("hello")}, {kServerHeartbeat, {}}, {kUnsequencedData, ascii("x")}};
  EXPECT_EQ(got, expect);
  EXPECT_EQ(p.buffered(), 0u);
}

TEST(FrameParser, HandlesManyPacketsInOneFeed) {
  Bytes stream;
  for (int i = 0; i < 1000; ++i) append_packet(stream, kSequencedData, ascii("payload"));
  FrameParser p;
  p.feed(stream);
  EXPECT_EQ(drain(p).size(), 1000u);
}

TEST(FrameParser, PartialPacketWaitsForMore) {
  Bytes stream;
  append_packet(stream, kSequencedData, ascii("hello"));
  FrameParser p;
  p.feed(std::span<const std::uint8_t>(stream.data(), 4));
  FrameParser::Status st;
  EXPECT_TRUE(drain(p, &st).empty());
  EXPECT_EQ(st, FrameParser::Status::NeedMore);
  p.feed(std::span<const std::uint8_t>(stream.data() + 4, stream.size() - 4));
  EXPECT_EQ(drain(p).size(), 1u);
}

TEST(FrameParser, ZeroLengthIsAStickyError) {
  FrameParser p;
  const Bytes bad = {0x00, 0x00, 'S'};
  p.feed(bad);
  char type = 0;
  std::span<const std::uint8_t> payload;
  EXPECT_EQ(p.next(type, payload), FrameParser::Status::Error);
  Bytes good;
  append_packet(good, kServerHeartbeat);
  p.feed(good);
  EXPECT_EQ(p.next(type, payload), FrameParser::Status::Error);
  p.reset();
  p.feed(good);
  EXPECT_EQ(p.next(type, payload), FrameParser::Status::Packet);
}

TEST(FrameParser, LongRunningStreamStaysBounded) {
  // Interleave feeds and drains so the internal buffer must compact itself.
  Bytes one;
  append_packet(one, kSequencedData, Bytes(100, 7));
  FrameParser p;
  std::size_t packets = 0;
  for (int i = 0; i < 20000; ++i) {
    p.feed(one);
    packets += drain(p).size();
  }
  EXPECT_EQ(packets, 20000u);
  EXPECT_EQ(p.buffered(), 0u);
}
