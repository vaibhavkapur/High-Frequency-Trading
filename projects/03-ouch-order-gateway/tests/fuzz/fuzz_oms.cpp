// libFuzzer target: the order state machine. The input is interpreted as a
// stream of framed OUCH messages (1 direction byte + 1 length byte + body)
// applied to an OrderManager in whatever order the fuzzer invents. Whatever
// arrives, the books must balance.
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

#include "ouch/order_manager.hpp"
#include "ouch/wire.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  ouch::OrderManager oms;
  std::size_t at = 0;
  std::uint64_t ts = 1;
  while (at + 2 <= size) {
    const std::uint8_t dir = data[at];
    const std::size_t len = data[at + 1];
    at += 2;
    if (at + len > size) break;
    const std::span<const std::uint8_t> body(data + at, len);
    at += len;
    if (dir == 0xFF) {
      oms.on_disconnect();
    } else if ((dir & 1) != 0) {
      ouch::wire::InboundMessage m;
      if (ouch::wire::decode_inbound(body, m) == ouch::wire::DecodeError::None) {
        oms.on_request_sent(m, ts++);
      }
    } else {
      ouch::wire::OutboundMessage m;
      if (ouch::wire::decode_outbound(body, m) == ouch::wire::DecodeError::None) {
        oms.on_exchange(m, ts++);
      }
    }
  }
  if (!oms.check_invariants().empty()) std::abort();
  return 0;
}
