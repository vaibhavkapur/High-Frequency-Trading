#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "itch/framing.hpp"

using namespace itch;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes mold_packet(std::uint64_t seq, std::uint16_t count, const std::vector<Bytes>& messages) {
  Bytes p{'S', 'E', 'S', 'S', 'I', 'O', 'N', '0', '0', '1'};
  p.resize(kMoldHeaderLength);
  store_be(p.data() + 10, seq, 8);
  store_be(p.data() + 18, count, 2);
  for (const Bytes& m : messages) {
    const std::size_t at = p.size();
    p.resize(at + 2);
    store_be(p.data() + at, m.size(), 2);
    p.insert(p.end(), m.begin(), m.end());
  }
  return p;
}

}  // namespace

TEST_CASE("length-prefixed reader yields payloads in order", "[framing]") {
  const Bytes data{0x00, 0x03, 'a', 'b', 'c', 0x00, 0x01, 'z'};
  LengthPrefixedReader r(data);
  std::span<const std::uint8_t> p;
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(Bytes(p.begin(), p.end()) == Bytes{'a', 'b', 'c'});
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(Bytes(p.begin(), p.end()) == Bytes{'z'});
  CHECK(r.next(p) == FrameStatus::End);
  CHECK(r.next(p) == FrameStatus::End);
  CHECK(r.offset() == data.size());
}

TEST_CASE("length prefix is big-endian", "[framing]") {
  Bytes data{0x01, 0x02};
  data.resize(2 + 0x0102, 0x55);
  LengthPrefixedReader r(data);
  std::span<const std::uint8_t> p;
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(p.size() == 0x0102);
}

TEST_CASE("empty input is a clean end", "[framing]") {
  LengthPrefixedReader r({});
  std::span<const std::uint8_t> p;
  CHECK(r.next(p) == FrameStatus::End);
}

TEST_CASE("truncated length prefix is terminal", "[framing][negative]") {
  const Bytes data{0x00, 0x01, 'a', 0x00};
  LengthPrefixedReader r(data);
  std::span<const std::uint8_t> p;
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(r.next(p) == FrameStatus::TruncatedLength);
  CHECK(r.next(p) == FrameStatus::TruncatedLength);
  CHECK(r.offset() == 3);
}

TEST_CASE("truncated payload is terminal and never yields a short span", "[framing][negative]") {
  const Bytes data{0x00, 0x01, 'a', 0x00, 0x05, 'b', 'c'};
  LengthPrefixedReader r(data);
  std::span<const std::uint8_t> p;
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(r.next(p) == FrameStatus::TruncatedPayload);
  CHECK(r.next(p) == FrameStatus::TruncatedPayload);
  CHECK(r.offset() == 3);
}

TEST_CASE("zero-length frame is skipped, not fatal", "[framing][negative]") {
  const Bytes data{0x00, 0x00, 0x00, 0x01, 'a'};
  LengthPrefixedReader r(data);
  std::span<const std::uint8_t> p;
  CHECK(r.next(p) == FrameStatus::ZeroLength);
  REQUIRE(r.next(p) == FrameStatus::Ok);
  CHECK(p.size() == 1);
  CHECK(r.next(p) == FrameStatus::End);
}

TEST_CASE("MoldUDP64 packet delivers each block with its sequence number", "[framing][mold]") {
  const Bytes pkt = mold_packet(100, 2, {{'a', 'b'}, {'c'}});
  MoldHeader hdr;
  std::vector<std::pair<std::uint64_t, Bytes>> got;
  const MoldStatus st =
      parse_moldudp64(pkt, hdr, [&](std::uint64_t seq, std::span<const std::uint8_t> m) {
        got.emplace_back(seq, Bytes(m.begin(), m.end()));
      });
  REQUIRE(st == MoldStatus::Ok);
  CHECK(hdr.sequence == 100);
  CHECK(hdr.count == 2);
  CHECK(hdr.session[0] == 'S');
  REQUIRE(got.size() == 2);
  CHECK(got[0] == std::pair<std::uint64_t, Bytes>{100, {'a', 'b'}});
  CHECK(got[1] == std::pair<std::uint64_t, Bytes>{101, {'c'}});
}

TEST_CASE("MoldUDP64 heartbeat, end of session and truncation", "[framing][mold][negative]") {
  MoldHeader hdr;
  int calls = 0;
  const auto count = [&](std::uint64_t, std::span<const std::uint8_t>) { ++calls; };

  CHECK(parse_moldudp64(mold_packet(5, kMoldHeartbeat, {}), hdr, count) == MoldStatus::Heartbeat);
  CHECK(parse_moldudp64(mold_packet(5, kMoldEndOfSession, {}), hdr, count) ==
        MoldStatus::EndOfSession);
  CHECK(calls == 0);

  const Bytes short_header(kMoldHeaderLength - 1, 0);
  CHECK(parse_moldudp64(short_header, hdr, count) == MoldStatus::TruncatedHeader);

  // Header claims two blocks but only one is present.
  CHECK(parse_moldudp64(mold_packet(1, 2, {{'a'}}), hdr, count) == MoldStatus::TruncatedBlock);
  CHECK(calls == 1);

  // Block length runs past the end of the packet.
  Bytes bad = mold_packet(1, 1, {{'a', 'b', 'c'}});
  bad.pop_back();
  calls = 0;
  CHECK(parse_moldudp64(bad, hdr, count) == MoldStatus::TruncatedBlock);
  CHECK(calls == 0);
}

TEST_CASE("sequence tracker drops duplicates and counts gaps", "[framing][mold]") {
  MoldSequenceTracker tracker;
  std::vector<std::uint64_t> seen;
  const auto record = [&](std::uint64_t seq, std::span<const std::uint8_t>) {
    seen.push_back(seq);
  };

  REQUIRE(tracker.on_packet(mold_packet(1, 2, {{'a'}, {'b'}}), record) == MoldStatus::Ok);
  // Redelivery overlapping one old and one new message.
  REQUIRE(tracker.on_packet(mold_packet(2, 2, {{'b'}, {'c'}}), record) == MoldStatus::Ok);
  CHECK(seen == std::vector<std::uint64_t>{1, 2, 3});
  CHECK(tracker.duplicates() == 1);
  CHECK(tracker.gaps() == 0);

  // Messages 4..6 lost.
  REQUIRE(tracker.on_packet(mold_packet(7, 1, {{'g'}}), record) == MoldStatus::Ok);
  CHECK(seen.back() == 7);
  CHECK(tracker.gaps() == 1);
  CHECK(tracker.missed_messages() == 3);
  CHECK(tracker.expected() == 8);

  // A heartbeat announcing a later sequence reveals a loss with no data packet.
  REQUIRE(tracker.on_packet(mold_packet(10, kMoldHeartbeat, {}), record) == MoldStatus::Heartbeat);
  CHECK(tracker.gaps() == 2);
  CHECK(tracker.missed_messages() == 5);
  CHECK(tracker.expected() == 10);

  // An in-sync heartbeat changes nothing.
  REQUIRE(tracker.on_packet(mold_packet(10, kMoldHeartbeat, {}), record) == MoldStatus::Heartbeat);
  CHECK(tracker.gaps() == 2);
}
