// me_latency: per-command latency distribution, sustained throughput and
// allocations per event for a generated mixed workload.
//
//   me_latency [--book pool|map] [--count N] [--warmup N] [--live N] [--symbols K]
//              [--depth-levels D] [--max-orders M] [--random-ids] [--seed S]
//              [--label NAME] [--json FILE]
//
// --live is the total number of resting orders the workload steers towards,
// spread uniformly over the symbols. --max-orders is the per-book capacity
// (default: 4 * live / symbols + 1024, i.e. sized for the expected occupancy).
//
// Method (see docs/BENCHMARKS.md):
//   * The whole command stream is generated up front; generation is never timed.
//   * Throughput pass: a fresh engine processes `warmup` commands untimed, then the
//     `count` measured commands inside one pair of clock reads. Allocations are
//     counted over the same window.
//   * Latency pass: a second fresh engine replays the same stream; each measured
//     command is bracketed by two clock reads and classified by what it did.
//   * The clock's granularity and read cost are measured and printed, because on
//     some machines (Apple Silicon: ~42 ns ticks) they are the same order of
//     magnitude as a single command.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "me/alloc_counter.hpp"
#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"
#include "me/workload.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

enum Class : std::size_t { AddPassive, AddAggressive, Cancel, Reduce, Replace, Reject, kClasses };
constexpr const char* kClassNames[kClasses] = {"add_passive", "add_aggressive", "cancel",
                                               "reduce",      "replace",        "reject"};

struct Options {
  std::string book = "pool";
  std::size_t count = 2'000'000;
  std::size_t warmup = 500'000;
  me::WorkloadConfig workload;
  std::uint32_t max_orders = 0;  // per book; 0 = derive from the workload
  std::string label = "default";
  std::string json;
};

struct Percentiles {
  std::size_t n = 0;
  std::int64_t p50 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0;
  double mean = 0;
};

Percentiles summarize(std::vector<std::int32_t>& v) {
  Percentiles p;
  p.n = v.size();
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  // Nearest-rank percentile: the smallest sample with at least q of the data at or below it.
  const auto at = [&v](double q) {
    const auto rank = static_cast<std::size_t>(q * static_cast<double>(v.size()) + 0.999999);
    return static_cast<std::int64_t>(v[std::min(v.size() - 1, rank == 0 ? 0 : rank - 1)]);
  };
  p.p50 = at(0.50);
  p.p95 = at(0.95);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = v.back();
  double sum = 0;
  for (const std::int32_t x : v) sum += x;
  p.mean = sum / static_cast<double>(v.size());
  return p;
}

struct ClockInfo {
  std::int64_t granularity_ns = 0;  // smallest non-zero difference between two reads
  double read_cost_ns = 0;          // mean cost of one clock read
};

ClockInfo probe_clock() {
  ClockInfo info;
  constexpr int kReads = 2'000'000;
  std::int64_t smallest = INT64_MAX;
  const std::int64_t start = now_ns();
  std::int64_t prev = start;
  for (int i = 0; i < kReads; ++i) {
    const std::int64_t t = now_ns();
    if (t != prev) smallest = std::min(smallest, t - prev);
    prev = t;
  }
  info.granularity_ns = smallest == INT64_MAX ? 0 : smallest;
  info.read_cost_ns = static_cast<double>(prev - start) / kReads;
  return info;
}

struct Result {
  double seconds = 0;
  double throughput = 0;
  std::uint64_t allocations = 0;
  std::uint64_t reports = 0;
  me::EngineStats stats;     // totals over warmup + measured
  me::EngineStats measured;  // measured window only
  std::size_t resting_orders = 0;
  std::size_t bid_levels = 0;
  std::size_t ask_levels = 0;
  Percentiles all;
  Percentiles by_class[kClasses];
};

me::EngineStats diff(const me::EngineStats& a, const me::EngineStats& b) {
  me::EngineStats d;
  d.commands = a.commands - b.commands;
  d.accepted = a.accepted - b.accepted;
  d.rejected = a.rejected - b.rejected;
  d.trades = a.trades - b.trades;
  d.canceled = a.canceled - b.canceled;
  d.reduced = a.reduced - b.reduced;
  d.replaced = a.replaced - b.replaced;
  d.traded_qty = a.traded_qty - b.traded_qty;
  return d;
}

template <class Book>
Result run(const Options& opt, const std::vector<me::Command>& commands) {
  me::EngineConfig config;
  config.symbols.resize(opt.workload.symbols);
  config.max_orders_per_book = opt.max_orders;
  Result result;

  {  // Throughput pass.
    me::Engine<Book> engine(config);
    std::uint64_t reports = 0;
    const auto sink = [&reports](const me::Report&) { ++reports; };
    for (std::size_t i = 0; i < opt.warmup; ++i) engine.process(commands[i], sink);
    const me::EngineStats before = engine.stats();
    reports = 0;
    const std::uint64_t allocs_before = me::alloc_count();
    const std::int64_t t0 = now_ns();
    for (std::size_t i = opt.warmup; i < commands.size(); ++i) engine.process(commands[i], sink);
    const std::int64_t t1 = now_ns();
    result.allocations = me::alloc_count() - allocs_before;
    result.seconds = static_cast<double>(t1 - t0) * 1e-9;
    result.throughput = static_cast<double>(opt.count) / result.seconds;
    result.reports = reports;
    result.stats = engine.stats();
    result.measured = diff(engine.stats(), before);
    for (me::SymbolId s = 0; s < engine.symbol_count(); ++s) {
      result.resting_orders += engine.book(s).size();
      result.bid_levels += engine.depth(s, me::Side::Buy, SIZE_MAX).size();
      result.ask_levels += engine.depth(s, me::Side::Sell, SIZE_MAX).size();
    }
  }

  {  // Latency pass.
    me::Engine<Book> engine(config);
    struct Sink {
      me::ReportType first{};
      std::uint32_t reports = 0;
      void operator()(const me::Report& r) {
        if (reports++ == 0) first = r.type;
      }
    } sink;
    for (std::size_t i = 0; i < opt.warmup; ++i) engine.process(commands[i], sink);

    std::vector<std::int32_t> all;
    all.reserve(opt.count);
    std::vector<std::uint8_t> cls;
    cls.reserve(opt.count);
    for (std::size_t i = opt.warmup; i < commands.size(); ++i) {
      sink.reports = 0;
      const std::int64_t t0 = now_ns();
      engine.process(commands[i], sink);
      const std::int64_t t1 = now_ns();
      Class c = Reject;
      switch (sink.first) {
        case me::ReportType::Accepted:
          c = sink.reports > 1 ? AddAggressive : AddPassive;
          break;
        case me::ReportType::Canceled:
          c = Cancel;
          break;
        case me::ReportType::Reduced:
          c = Reduce;
          break;
        case me::ReportType::Replaced:
          c = Replace;
          break;
        default:
          c = Reject;
          break;
      }
      all.push_back(static_cast<std::int32_t>(std::min<std::int64_t>(t1 - t0, INT32_MAX)));
      cls.push_back(static_cast<std::uint8_t>(c));
    }
    std::vector<std::int32_t> split[kClasses];
    for (std::size_t i = 0; i < all.size(); ++i) split[cls[i]].push_back(all[i]);
    for (std::size_t c = 0; c < kClasses; ++c) result.by_class[c] = summarize(split[c]);
    result.all = summarize(all);
  }
  return result;
}

void print_row(const char* name, const Percentiles& p) {
  std::printf("  %-15s %10zu %8lld %8lld %8lld %8lld %10lld %9.1f\n", name, p.n, static_cast<long long>(p.p50),
              static_cast<long long>(p.p95), static_cast<long long>(p.p99), static_cast<long long>(p.p999),
              static_cast<long long>(p.max), p.mean);
}

void json_percentiles(std::FILE* f, const char* name, const Percentiles& p, bool last) {
  std::fprintf(f,
               "    \"%s\": {\"count\": %zu, \"p50\": %lld, \"p95\": %lld, \"p99\": %lld, \"p99_9\": %lld, "
               "\"max\": %lld, \"mean\": %.2f}%s\n",
               name, p.n, static_cast<long long>(p.p50), static_cast<long long>(p.p95), static_cast<long long>(p.p99),
               static_cast<long long>(p.p999), static_cast<long long>(p.max), p.mean, last ? "" : ",");
}

template <class T>
bool parse(const char* s, T& out) {
  const char* const end = s + std::strlen(s);
  const auto [ptr, ec] = std::from_chars(s, end, out);
  return ec == std::errc{} && ptr == end;
}

int usage() {
  std::fputs(
      "usage: me_latency [--book pool|map] [--count N] [--warmup N] [--live N] [--symbols K]\n"
      "                  [--depth-levels D] [--max-orders M] [--random-ids] [--seed S]\n"
      "                  [--label NAME] [--json FILE]\n",
      stderr);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--random-ids") {
      opt.workload.random_ids = true;
      continue;
    }
    if (i + 1 >= argc) return usage();
    const char* v = argv[++i];
    bool ok = true;
    if (a == "--book") {
      opt.book = v;
    } else if (a == "--label") {
      opt.label = v;
    } else if (a == "--json") {
      opt.json = v;
    } else if (a == "--count") {
      ok = parse(v, opt.count);
    } else if (a == "--warmup") {
      ok = parse(v, opt.warmup);
    } else if (a == "--live") {
      ok = parse(v, opt.workload.target_live);
    } else if (a == "--symbols") {
      ok = parse(v, opt.workload.symbols);
    } else if (a == "--depth-levels") {
      ok = parse(v, opt.workload.depth_levels);
    } else if (a == "--max-orders") {
      ok = parse(v, opt.max_orders);
    } else if (a == "--seed") {
      ok = parse(v, opt.workload.seed);
    } else {
      ok = false;
    }
    if (!ok) return usage();
  }
  if (opt.book != "pool" && opt.book != "map") return usage();
  if (opt.count == 0 || opt.workload.symbols == 0) return usage();
  if (opt.max_orders == 0) opt.max_orders = 4 * (opt.workload.target_live / opt.workload.symbols) + 1024;

  std::vector<me::Command> commands;
  try {
    commands = me::Workload(opt.workload).generate(opt.warmup + opt.count);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }

  const ClockInfo clock = probe_clock();
  const Result r = opt.book == "pool" ? run<me::PoolBook>(opt, commands) : run<me::MapBook>(opt, commands);

  const me::EngineStats& m = r.measured;
  std::printf("label=%s book=%s symbols=%u target_live=%u depth_levels=%u max_orders_per_book=%u ids=%s seed=%llu\n",
              opt.label.c_str(), opt.book.c_str(), opt.workload.symbols, opt.workload.target_live,
              opt.workload.depth_levels, opt.max_orders, opt.workload.random_ids ? "random" : "sequential",
              static_cast<unsigned long long>(opt.workload.seed));
  std::printf("warmup=%zu measured=%zu  clock: granularity=%lld ns, read cost=%.1f ns\n", opt.warmup, opt.count,
              static_cast<long long>(clock.granularity_ns), clock.read_cost_ns);
  std::printf("measured mix: accepted=%llu trades=%llu canceled=%llu reduced=%llu replaced=%llu rejected=%llu\n",
              static_cast<unsigned long long>(m.accepted), static_cast<unsigned long long>(m.trades),
              static_cast<unsigned long long>(m.canceled), static_cast<unsigned long long>(m.reduced),
              static_cast<unsigned long long>(m.replaced), static_cast<unsigned long long>(m.rejected));
  std::printf("end state: resting_orders=%zu bid_levels=%zu ask_levels=%zu\n", r.resting_orders, r.bid_levels,
              r.ask_levels);
  std::printf("throughput: %.0f commands/s (%.1f ns/command mean, %llu reports)\n", r.throughput, 1e9 / r.throughput,
              static_cast<unsigned long long>(r.reports));
  std::printf("allocations: %llu in measured window (%.4f per command)\n",
              static_cast<unsigned long long>(r.allocations),
              static_cast<double>(r.allocations) / static_cast<double>(opt.count));
  std::printf("latency per command, ns (includes one clock read, ~%.0f ns):\n", clock.read_cost_ns);
  std::printf("  %-15s %10s %8s %8s %8s %8s %10s %9s\n", "class", "count", "p50", "p95", "p99", "p99.9", "max", "mean");
  print_row("all", r.all);
  for (std::size_t c = 0; c < kClasses; ++c) print_row(kClassNames[c], r.by_class[c]);

  if (!opt.json.empty()) {
    std::FILE* f = std::fopen(opt.json.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "error: cannot open %s\n", opt.json.c_str());
      return 2;
    }
    std::fprintf(f, "{\n  \"label\": \"%s\",\n  \"book\": \"%s\",\n", opt.label.c_str(), opt.book.c_str());
    std::fprintf(f,
                 "  \"workload\": {\"seed\": %llu, \"symbols\": %u, \"target_live\": %u, \"depth_levels\": %u, "
                 "\"random_ids\": %s, \"add_pm\": %u, \"cancel_pm\": %u, \"reduce_pm\": %u, \"replace_pm\": %u, "
                 "\"aggressive_pm\": %u},\n",
                 static_cast<unsigned long long>(opt.workload.seed), opt.workload.symbols, opt.workload.target_live,
                 opt.workload.depth_levels, opt.workload.random_ids ? "true" : "false", opt.workload.add_pm,
                 opt.workload.cancel_pm, opt.workload.reduce_pm, opt.workload.replace_pm, opt.workload.aggressive_pm);
    std::fprintf(f, "  \"max_orders_per_book\": %u,\n", opt.max_orders);
    std::fprintf(f, "  \"warmup_commands\": %zu,\n  \"measured_commands\": %zu,\n", opt.warmup, opt.count);
    std::fprintf(f, "  \"clock\": {\"granularity_ns\": %lld, \"read_cost_ns\": %.2f},\n",
                 static_cast<long long>(clock.granularity_ns), clock.read_cost_ns);
    std::fprintf(f,
                 "  \"measured_mix\": {\"accepted\": %llu, \"trades\": %llu, \"canceled\": %llu, \"reduced\": %llu, "
                 "\"replaced\": %llu, \"rejected\": %llu, \"reports\": %llu},\n",
                 static_cast<unsigned long long>(m.accepted), static_cast<unsigned long long>(m.trades),
                 static_cast<unsigned long long>(m.canceled), static_cast<unsigned long long>(m.reduced),
                 static_cast<unsigned long long>(m.replaced), static_cast<unsigned long long>(m.rejected),
                 static_cast<unsigned long long>(r.reports));
    std::fprintf(f, "  \"end_state\": {\"resting_orders\": %zu, \"bid_levels\": %zu, \"ask_levels\": %zu},\n",
                 r.resting_orders, r.bid_levels, r.ask_levels);
    std::fprintf(f, "  \"throughput_commands_per_sec\": %.0f,\n  \"elapsed_seconds\": %.6f,\n", r.throughput,
                 r.seconds);
    std::fprintf(f, "  \"allocations\": %llu,\n  \"allocations_per_command\": %.6f,\n",
                 static_cast<unsigned long long>(r.allocations),
                 static_cast<double>(r.allocations) / static_cast<double>(opt.count));
    std::fprintf(f, "  \"latency_ns\": {\n");
    json_percentiles(f, "all", r.all, false);
    for (std::size_t c = 0; c < kClasses; ++c) json_percentiles(f, kClassNames[c], r.by_class[c], c + 1 == kClasses);
    std::fprintf(f, "  }\n}\n");
    std::fclose(f);
  }
  return 0;
}
