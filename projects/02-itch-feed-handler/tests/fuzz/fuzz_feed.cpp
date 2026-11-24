// libFuzzer target: treats the input as a length-prefixed ITCH file and runs
// it through framing, decoding and book building.
//
//   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DITCH_BUILD_FUZZERS=ON \
//         -DITCH_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
//   cmake --build build-fuzz --target fuzz_feed
//   ./build-fuzz/fuzz_feed -max_len=4096
//
// Needs an LLVM clang that ships libFuzzer; Apple's Xcode clang does not.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  itch::FeedHandler<> handler;
  itch::LengthPrefixedReader reader(std::span<const std::uint8_t>(data, size));
  std::span<const std::uint8_t> payload;
  for (;;) {
    const itch::FrameStatus fs = reader.next(payload);
    if (fs == itch::FrameStatus::ZeroLength) continue;
    if (fs != itch::FrameStatus::Ok) break;
    (void)handler.process(payload);
  }
  const itch::FeedCounters& c = handler.counters();
  if (c.messages !=
      c.decoded + c.empty + c.unknown_type + c.truncated + c.oversized + c.invalid_field) {
    std::abort();
  }
  (void)handler.books().digest();
  return 0;
}
