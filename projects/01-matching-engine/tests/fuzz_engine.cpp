// libFuzzer entry point for the binary command path (build with -DME_BUILD_FUZZERS=ON
// using a clang that ships libFuzzer; Apple clang does not).
//
// The input is interpreted as a raw command stream. Both books must stay
// structurally valid and must emit identical reports for any byte sequence.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  me::EngineConfig config;
  me::SymbolConfig symbol;
  symbol.tick_size = 2;
  symbol.lot_size = 3;
  config.symbols.assign(2, symbol);
  config.max_orders_per_book = 32;
  me::Engine<me::MapBook> map_engine(config);
  me::Engine<me::PoolBook> pool_engine(config);

  std::vector<me::Report> map_out;
  std::vector<me::Report> pool_out;
  for (std::size_t off = 0; off + me::kCommandSize <= size; off += me::kCommandSize) {
    const auto* bytes = reinterpret_cast<const std::byte*>(data + off);
    me::Command c = me::decode_command(std::span<const std::byte, me::kCommandSize>(bytes, me::kCommandSize));
    // Fold ids and prices into a small range so the fuzzer finds interacting orders quickly.
    c.order_id %= 64;
    c.new_order_id %= 64;
    c.price %= 64;
    c.qty %= 64;
    map_out.clear();
    pool_out.clear();
    map_engine.process(c, [&map_out](const me::Report& r) { map_out.push_back(r); });
    pool_engine.process(c, [&pool_out](const me::Report& r) { pool_out.push_back(r); });
    if (map_out != pool_out) std::abort();
  }
  for (me::SymbolId s = 0; s < 2; ++s) {
    if (!map_engine.book(s).check_invariants() || !pool_engine.book(s).check_invariants()) std::abort();
  }
  return 0;
}
