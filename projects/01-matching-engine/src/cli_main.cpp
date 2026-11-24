// me_cli: command-line front end for the matching engine.
//
//   me_cli gen     --out FILE [--count N] [--seed S] [--symbols K] [--tick T] [--lot L]
//                  [--live N] [--depth-levels D] [--invalid-pm P] [--random-ids]
//   me_cli encode  --in FILE --out FILE              text commands -> binary commands
//   me_cli replay  --in FILE --out FILE [--book pool|map] [--symbols K] [--tick T] [--lot L]
//                  [--max-orders M] [--text-in] [--text] [--depth N]
//   me_cli dump    --in FILE [--commands]            binary reports (or commands) -> text
//
// FILE may be "-" for stdin/stdout. replay is streaming: it processes each
// command as soon as it has been read and never holds the whole log in memory.
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"
#include "me/text_format.hpp"
#include "me/workload.hpp"

namespace {

using me::Command;
using me::Report;

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Args {
 public:
  Args(int argc, char** argv, int first) {
    for (int i = first; i < argc; ++i) {
      const std::string_view a = argv[i];
      if (a.size() < 3 || a.substr(0, 2) != "--") throw UsageError("unexpected argument '" + std::string(a) + "'");
      const std::string key(a.substr(2));
      if (is_flag(key)) {
        values_[key] = "1";
      } else {
        if (i + 1 >= argc) throw UsageError("missing value for --" + key);
        values_[key] = argv[++i];
      }
    }
  }

  bool flag(const std::string& key) {
    used(key);
    return values_.count(key) != 0;
  }

  std::string str(const std::string& key, const char* fallback = nullptr) {
    used(key);
    const auto it = values_.find(key);
    if (it != values_.end()) return it->second;
    if (fallback == nullptr) throw UsageError("missing required option --" + key);
    return fallback;
  }

  template <class T>
  T num(const std::string& key, T fallback) {
    used(key);
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    T v{};
    const char* const end = it->second.data() + it->second.size();
    const auto [ptr, ec] = std::from_chars(it->second.data(), end, v);
    if (ec != std::errc{} || ptr != end) throw UsageError("bad value for --" + key + ": " + it->second);
    return v;
  }

  void finish() const {
    for (const auto& [key, value] : values_) {
      if (seen_.count(key) == 0) throw UsageError("unknown option --" + key);
    }
  }

 private:
  static bool is_flag(const std::string& key) {
    return key == "text" || key == "text-in" || key == "random-ids" || key == "commands";
  }
  void used(const std::string& key) { seen_[key] = true; }

  std::map<std::string, std::string> values_;
  std::map<std::string, bool> seen_;
};

class File {
 public:
  File(const std::string& path, bool write) : owned_(path != "-") {
    f_ = owned_ ? std::fopen(path.c_str(), write ? "wb" : "rb") : (write ? stdout : stdin);
    if (f_ == nullptr) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() {
    if (f_ != nullptr && owned_) std::fclose(f_);
  }

  std::FILE* get() const { return f_; }

  void write(const void* data, std::size_t size) {
    if (std::fwrite(data, 1, size, f_) != size) throw std::runtime_error("write failed");
  }
  void write_line(const std::string& s) {
    write(s.data(), s.size());
    write("\n", 1);
  }
  void close() {
    const bool failed = std::fflush(f_) != 0 || (owned_ && std::fclose(f_) != 0);
    f_ = nullptr;
    if (failed) throw std::runtime_error("write failed");
  }

 private:
  bool owned_;
  std::FILE* f_;
};

// Reads '\n'-terminated lines of any length. Returns false at end of input.
bool read_line(std::FILE* f, std::string& line) {
  line.clear();
  int ch;
  while ((ch = std::fgetc(f)) != EOF) {
    if (ch == '\n') return true;
    line.push_back(static_cast<char>(ch));
  }
  return !line.empty();
}

int cmd_gen(Args& args) {
  me::WorkloadConfig cfg;
  const auto count = args.num<std::size_t>("count", 1'000'000);
  cfg.seed = args.num("seed", cfg.seed);
  cfg.symbols = args.num("symbols", cfg.symbols);
  cfg.tick_size = args.num("tick", cfg.tick_size);
  cfg.lot_size = args.num("lot", cfg.lot_size);
  cfg.target_live = args.num("live", cfg.target_live);
  cfg.depth_levels = args.num("depth-levels", cfg.depth_levels);
  cfg.invalid_pm = args.num("invalid-pm", cfg.invalid_pm);
  cfg.random_ids = args.flag("random-ids");
  File out(args.str("out"), true);
  args.finish();

  me::Workload workload(cfg);
  for (std::size_t i = 0; i < count; ++i) {
    const Command c = workload.next();
    out.write(&c, sizeof c);
  }
  out.close();
  std::fprintf(stderr, "generated %zu commands (%zu bytes)\n", count, count * me::kCommandSize);
  return 0;
}

int cmd_encode(Args& args) {
  File in(args.str("in"), false);
  File out(args.str("out"), true);
  args.finish();

  std::string line;
  std::string error;
  std::size_t line_no = 0;
  std::size_t count = 0;
  while (read_line(in.get(), line)) {
    ++line_no;
    Command c;
    switch (me::parse_command(line, c, &error)) {
      case me::ParseStatus::Blank:
        break;
      case me::ParseStatus::Error:
        std::fprintf(stderr, "line %zu: %s\n", line_no, error.c_str());
        return 2;
      case me::ParseStatus::Ok:
        out.write(&c, sizeof c);
        ++count;
        break;
    }
  }
  out.close();
  std::fprintf(stderr, "encoded %zu commands\n", count);
  return 0;
}

int cmd_dump(Args& args) {
  File in(args.str("in"), false);
  const bool commands = args.flag("commands");
  args.finish();

  File out("-", true);
  const std::size_t record = commands ? me::kCommandSize : me::kReportSize;
  std::array<std::byte, me::kReportSize> buf{};
  std::size_t got;
  while ((got = std::fread(buf.data(), 1, record, in.get())) == record) {
    if (commands) {
      out.write_line(me::format_command(
          me::decode_command(std::span<const std::byte, me::kCommandSize>(buf.data(), me::kCommandSize))));
    } else {
      out.write_line(me::format_report(me::decode_report(buf)));
    }
  }
  out.close();
  if (got != 0) {
    std::fprintf(stderr, "error: input ends with a truncated %zu-byte record\n", got);
    return 2;
  }
  return 0;
}

struct ReplayOptions {
  bool text_in = false;
  bool text_out = false;
  std::size_t depth = 0;
};

void print_side(const char* label, const std::vector<me::LevelView>& levels) {
  std::fprintf(stderr, "  %s:", label);
  if (levels.empty()) std::fprintf(stderr, " (empty)");
  for (const me::LevelView& l : levels) {
    std::fprintf(stderr, " %lld x %lld (%u)", static_cast<long long>(l.price), static_cast<long long>(l.qty), l.orders);
  }
  std::fprintf(stderr, "\n");
}

template <class Book>
int replay(const me::EngineConfig& config, File& in, File& out, const ReplayOptions& opt) {
  me::Engine<Book> engine(config);
  std::uint64_t reports = 0;
  const auto sink = [&](const Report& r) {
    ++reports;
    if (opt.text_out) {
      out.write_line(me::format_report(r));
    } else {
      out.write(&r, sizeof r);
    }
  };

  int status = 0;
  const auto start = std::chrono::steady_clock::now();
  if (opt.text_in) {
    std::string line;
    std::string error;
    std::size_t line_no = 0;
    while (read_line(in.get(), line)) {
      ++line_no;
      Command c;
      const me::ParseStatus st = me::parse_command(line, c, &error);
      if (st == me::ParseStatus::Error) {
        std::fprintf(stderr, "line %zu: %s\n", line_no, error.c_str());
        status = 2;
        break;
      }
      if (st == me::ParseStatus::Ok) engine.process(c, sink);
    }
  } else {
    std::array<std::byte, me::kCommandSize> buf{};
    std::size_t got;
    while ((got = std::fread(buf.data(), 1, buf.size(), in.get())) == buf.size()) {
      engine.process(me::decode_command(buf), sink);
    }
    if (got != 0) {
      std::fprintf(stderr, "error: input ends with a truncated %zu-byte command\n", got);
      status = 2;
    }
  }
  out.close();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const me::EngineStats& s = engine.stats();
  std::fprintf(stderr,
               "book=%s commands=%llu reports=%llu accepted=%llu rejected=%llu trades=%llu "
               "canceled=%llu reduced=%llu replaced=%llu traded_qty=%lld\n",
               Book::name(), static_cast<unsigned long long>(s.commands), static_cast<unsigned long long>(reports),
               static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.rejected),
               static_cast<unsigned long long>(s.trades), static_cast<unsigned long long>(s.canceled),
               static_cast<unsigned long long>(s.reduced), static_cast<unsigned long long>(s.replaced),
               static_cast<long long>(s.traded_qty));
  std::fprintf(stderr, "elapsed=%.3fs (%.0f commands/s end to end, including file I/O)\n", seconds,
               seconds > 0 ? static_cast<double>(s.commands) / seconds : 0.0);
  if (opt.depth > 0) {
    for (me::SymbolId sym = 0; sym < engine.symbol_count(); ++sym) {
      std::fprintf(stderr, "symbol %u: %zu resting orders; price x qty (orders), best first\n", sym,
                   engine.book(sym).size());
      print_side("asks", engine.depth(sym, me::Side::Sell, opt.depth));
      print_side("bids", engine.depth(sym, me::Side::Buy, opt.depth));
    }
  }
  return status;
}

int cmd_replay(Args& args) {
  me::SymbolConfig symbol;
  symbol.tick_size = args.num("tick", symbol.tick_size);
  symbol.lot_size = args.num("lot", symbol.lot_size);
  me::EngineConfig config;
  config.symbols.assign(args.num<std::size_t>("symbols", 1), symbol);
  config.max_orders_per_book = args.num("max-orders", config.max_orders_per_book);
  ReplayOptions opt;
  opt.text_in = args.flag("text-in");
  opt.text_out = args.flag("text");
  opt.depth = args.num<std::size_t>("depth", 0);
  const std::string book = args.str("book", "pool");
  File in(args.str("in"), false);
  File out(args.str("out"), true);
  args.finish();

  if (book == "pool") return replay<me::PoolBook>(config, in, out, opt);
  if (book == "map") return replay<me::MapBook>(config, in, out, opt);
  throw UsageError("--book must be 'pool' or 'map'");
}

void usage() {
  std::fputs(
      "usage:\n"
      "  me_cli gen    --out FILE [--count N] [--seed S] [--symbols K] [--tick T] [--lot L]\n"
      "                [--live N] [--depth-levels D] [--invalid-pm P] [--random-ids]\n"
      "  me_cli encode --in FILE --out FILE\n"
      "  me_cli replay --in FILE --out FILE [--book pool|map] [--symbols K] [--tick T] [--lot L]\n"
      "                [--max-orders M] [--text-in] [--text] [--depth N]\n"
      "  me_cli dump   --in FILE [--commands]\n"
      "FILE may be '-' for stdin/stdout.\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  const std::string_view cmd = argv[1];
  try {
    Args args(argc, argv, 2);
    if (cmd == "gen") return cmd_gen(args);
    if (cmd == "encode") return cmd_encode(args);
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "dump") return cmd_dump(args);
    usage();
    return 1;
  } catch (const UsageError& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    usage();
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  }
}
