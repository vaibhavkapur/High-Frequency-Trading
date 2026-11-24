// libFuzzer target: OUCH decoders. Anything accepted must re-encode to the
// identical bytes. Build with -DOUCH_BUILD_FUZZERS=ON (needs clang with libFuzzer).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ouch/wire.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);
  std::array<std::uint8_t, ouch::wire::kMaxMessageSize> buf{};

  ouch::wire::InboundMessage inbound;
  if (ouch::wire::decode_inbound(in, inbound) == ouch::wire::DecodeError::None) {
    const std::size_t n = ouch::wire::encode(inbound, buf);
    if (n != size || !std::equal(in.begin(), in.end(), buf.begin())) std::abort();
  }
  ouch::wire::OutboundMessage outbound;
  if (ouch::wire::decode_outbound(in, outbound) == ouch::wire::DecodeError::None) {
    const std::size_t n = ouch::wire::encode(outbound, buf);
    if (n != size || !std::equal(in.begin(), in.end(), buf.begin())) std::abort();
  }
  return 0;
}
