// itch_gen - write a deterministic synthetic ITCH 5.0 file (length-prefixed).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include "itch/synth.hpp"

namespace {

void usage() {
  std::fputs(
      "usage: itch_gen --out FILE [--messages N] [--symbols N] [--seed N]\n"
      "                [--profile balanced|add-heavy|churn]\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  itch::SynthConfig cfg;
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--out") {
      out_path = value();
    } else if (arg == "--messages") {
      cfg.messages = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--symbols") {
      cfg.symbols = static_cast<std::uint32_t>(std::strtoul(value(), nullptr, 10));
    } else if (arg == "--seed") {
      cfg.seed = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--profile") {
      if (!itch::parse_profile(value(), cfg.profile)) {
        usage();
        return 2;
      }
    } else {
      usage();
      return 2;
    }
  }
  if (out_path.empty()) {
    usage();
    return 2;
  }

  const std::vector<std::uint8_t> data = itch::generate_synthetic(cfg);
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) {
    std::fprintf(stderr, "itch_gen: failed to write %s\n", out_path.c_str());
    return 1;
  }
  std::printf("wrote %s: %zu bytes, profile=%s messages=%llu symbols=%u seed=%llu\n",
              out_path.c_str(), data.size(), itch::to_string(cfg.profile),
              static_cast<unsigned long long>(cfg.messages), cfg.symbols,
              static_cast<unsigned long long>(cfg.seed));
  return 0;
}
