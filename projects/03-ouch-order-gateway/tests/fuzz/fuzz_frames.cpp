// libFuzzer target: SoupBinTCP reassembly and login parsing. The first input
// byte chooses the chunk size, so the same stream is exercised under many
// different TCP segmentations; the parser must never consume more than it was fed.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ouch/soupbin.hpp"
#include "ouch/wire.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  const std::size_t chunk = static_cast<std::size_t>(data[0]) + 1;
  const std::span<const std::uint8_t> stream(data + 1, size - 1);

  ouch::soup::FrameParser parser;
  std::size_t fed = 0;
  std::size_t consumed = 0;
  while (fed < stream.size()) {
    const std::size_t n = std::min(chunk, stream.size() - fed);
    parser.feed(stream.subspan(fed, n));
    fed += n;
    char type = 0;
    std::span<const std::uint8_t> payload;
    while (parser.next(type, payload) == ouch::soup::FrameParser::Status::Packet) {
      consumed += payload.size() + 3;
      if (consumed > fed) std::abort();
      ouch::soup::LoginRequest req;
      ouch::soup::LoginAccepted acc;
      (void)ouch::soup::parse_login_request(payload, req);
      (void)ouch::soup::parse_login_accepted(payload, acc);
      ouch::wire::OutboundMessage out;
      (void)ouch::wire::decode_outbound(payload, out);
    }
  }
  return 0;
}
