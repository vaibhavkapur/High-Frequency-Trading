// Deterministic synthetic ITCH 5.0 stream generator.
//
// Produces a length-prefixed file image that is *structurally* valid ITCH:
// every order operation refers to a live order, books never cross, and
// timestamps are non-decreasing. It is test and benchmark input, not a model
// of real market microstructure - see docs/benchmarks.md for what that means
// for the published numbers.
//
// Output depends only on the config (own PRNG, no <random> distributions), so
// the same config yields byte-identical files on every platform.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace itch {

enum class SynthProfile : std::uint8_t {
  Balanced,  // mixed adds / executes / cancels / deletes / replaces / trades
  AddHeavy,  // book build-up: mostly adds, the live order set keeps growing
  Churn,     // quote churn: build a standing book, then add and delete in equal measure
};

struct SynthConfig {
  std::uint64_t messages = 1'000'000;  // order-flow messages; a few admin messages are added
  std::uint32_t symbols = 100;         // stock locates 1..symbols
  std::uint64_t seed = 1;
  SynthProfile profile = SynthProfile::Balanced;
};

[[nodiscard]] bool parse_profile(std::string_view name, SynthProfile& out) noexcept;
[[nodiscard]] const char* to_string(SynthProfile p) noexcept;

// Returns the complete file image (2-byte big-endian length before each message).
[[nodiscard]] std::vector<std::uint8_t> generate_synthetic(const SynthConfig& cfg);

}  // namespace itch
