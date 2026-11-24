// Deterministic synthetic command generator shared by tests, benchmarks and the CLI.
//
// The generator uses its own integer-only PRNG (splitmix64), so a given
// WorkloadConfig produces the same command stream on every platform and standard
// library. It does not run a matching engine: it tracks the orders it has issued,
// so some cancels/reduces/replaces target orders that have already been filled
// and are rejected by the engine, as happens on a real venue.
#pragma once

#include <cstdint>
#include <vector>

#include "me/protocol.hpp"

namespace me {

struct WorkloadConfig {
  std::uint64_t seed = 1;
  std::uint32_t symbols = 1;
  Price tick_size = 1;
  Qty lot_size = 1;
  Price mid_ticks = 100'000;           // every symbol's book is centred on this many ticks
  std::uint32_t depth_levels = 64;     // passive orders are spread over this many ticks per side
  std::uint32_t max_lots = 100;        // order size is uniform in [1, max_lots] lots
  std::uint32_t target_live = 10'000;  // total resting orders (across symbols) the mix steers towards

  // Action mix in per-mille while below target_live; add and cancel weights swap above it.
  std::uint32_t add_pm = 600;
  std::uint32_t cancel_pm = 300;
  std::uint32_t reduce_pm = 50;
  std::uint32_t replace_pm = 50;
  std::uint32_t aggressive_pm = 100;  // share of new orders priced through the mid
  std::uint32_t invalid_pm = 0;       // share of commands deliberately malformed (tests)

  bool random_ids = false;  // false: ids 1,2,3,...  true: unique pseudo-random 64-bit ids
};

class Workload {
 public:
  explicit Workload(const WorkloadConfig& config);

  Command next();
  std::vector<Command> generate(std::size_t count);

 private:
  struct Live {
    SymbolId symbol;
    OrderId id;
    Qty lots;
  };

  std::uint64_t rand();
  std::uint64_t below(std::uint64_t n);
  OrderId new_id();
  Price passive_price(Side side);
  Price aggressive_price(Side side);
  Command make_new();
  Command make_invalid();

  WorkloadConfig cfg_;
  std::uint64_t state_;
  std::uint64_t id_counter_ = 0;
  std::vector<Live> live_;
};

}  // namespace me
