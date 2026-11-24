// itch_bench - one benchmark configuration per process, one JSON object out.
//
// Modes isolate the pipeline stages so their costs can be separated:
//   frame      walk the 2-byte length prefixes only
//   decode     framing + full message decode (parser only)
//   book-flat  framing + decode + book update, FlatOrderIndex
//   book-std   framing + decode + book update, StdOrderIndex (unordered_map)
//
// One process per mode keeps peak RSS and allocation counts attributable to a
// single configuration. Methodology and caveats: docs/benchmarks.md.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"
#include "itch/mapped_file.hpp"

#ifndef ITCH_BUILD_FLAGS
#define ITCH_BUILD_FLAGS "unknown"
#endif

// ---- allocation counting ----------------------------------------------------
// Replaces the global allocation functions for this binary only. Single
// threaded, so a plain counter is enough.
namespace {
std::uint64_t g_allocations = 0;
std::uint64_t g_allocated_bytes = 0;
}  // namespace

void* operator new(std::size_t size) {
  ++g_allocations;
  g_allocated_bytes += size;
  if (void* p = std::malloc(size != 0 ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;

// Forces the compiler to materialise `value`, so a decoded message cannot be
// optimised away just because the benchmark does not use its fields.
template <class T>
inline void do_not_optimize(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct Sink {
  template <class T>
  void operator()(const T& m) const noexcept {
    do_not_optimize(m);
  }
};

struct PassResult {
  std::uint64_t messages = 0;
  std::uint64_t checksum = 0;  // mode-specific value that depends on all work done
  std::uint64_t live_orders = 0;
  // Wall time of the message loop only: excludes the end-of-run digest and
  // tearing down the book, which a live handler never does per message.
  double seconds = 0.0;
};

// Each pass owns its state and reports whether a per-message callback wants
// timing. `Hook` is called around each message.
template <class Hook>
PassResult pass_frame(std::span<const std::uint8_t> data, Hook&& hook) {
  PassResult r;
  itch::LengthPrefixedReader reader(data);
  std::span<const std::uint8_t> payload;
  const auto loop_start = Clock::now();
  for (;;) {
    hook.begin();
    const itch::FrameStatus fs = reader.next(payload);
    if (fs == itch::FrameStatus::ZeroLength) continue;
    if (fs != itch::FrameStatus::Ok) break;
    r.checksum += payload.size() + payload[0];
    hook.end();
    ++r.messages;
  }
  r.seconds = std::chrono::duration<double>(Clock::now() - loop_start).count();
  return r;
}

template <class Hook>
PassResult pass_decode(std::span<const std::uint8_t> data, Hook&& hook) {
  PassResult r;
  itch::LengthPrefixedReader reader(data);
  std::span<const std::uint8_t> payload;
  const auto loop_start = Clock::now();
  for (;;) {
    hook.begin();
    const itch::FrameStatus fs = reader.next(payload);
    if (fs == itch::FrameStatus::ZeroLength) continue;
    if (fs != itch::FrameStatus::Ok) break;
    r.checksum += static_cast<std::uint64_t>(itch::decode(payload, Sink{}));
    hook.end();
    ++r.messages;
  }
  r.seconds = std::chrono::duration<double>(Clock::now() - loop_start).count();
  return r;
}

template <class Index, class Hook>
PassResult pass_book(std::span<const std::uint8_t> data, Hook&& hook) {
  PassResult r;
  itch::FeedHandler<Index> handler;
  itch::LengthPrefixedReader reader(data);
  std::span<const std::uint8_t> payload;
  const auto loop_start = Clock::now();
  for (;;) {
    hook.begin();
    const itch::FrameStatus fs = reader.next(payload);
    if (fs == itch::FrameStatus::ZeroLength) continue;
    if (fs != itch::FrameStatus::Ok) break;
    const itch::ProcessResult pr = handler.process(payload);
    do_not_optimize(pr);
    hook.end();
    ++r.messages;
  }
  r.seconds = std::chrono::duration<double>(Clock::now() - loop_start).count();
  r.checksum = handler.books().digest();
  r.live_orders = handler.books().live_orders();
  return r;
}

struct NoHook {
  void begin() const noexcept {}
  void end() const noexcept {}
};

// Records one steady_clock delta per message.
struct LatencyHook {
  std::vector<std::uint32_t>* samples;
  Clock::time_point t0{};
  void begin() noexcept { t0 = Clock::now(); }
  void end() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    samples->push_back(static_cast<std::uint32_t>(std::min<long long>(ns, 0xFFFFFFFFll)));
  }
};

template <class Hook>
PassResult run_pass(std::string_view mode, std::span<const std::uint8_t> data, Hook&& hook) {
  if (mode == "frame") return pass_frame(data, hook);
  if (mode == "decode") return pass_decode(data, hook);
  if (mode == "book-flat") return pass_book<itch::FlatOrderIndex>(data, hook);
  return pass_book<itch::StdOrderIndex>(data, hook);
}

std::uint32_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto rank = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
  return sorted[rank];
}

// Smallest non-zero difference between consecutive clock reads, and the
// median cost of a back-to-back read pair. Per-message latencies cannot be
// resolved below the first and include roughly the second.
void probe_clock(std::uint32_t& min_nonzero_ns, std::uint32_t& median_pair_ns) {
  std::vector<std::uint32_t> d;
  d.reserve(200'000);
  for (int i = 0; i < 200'000; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    d.push_back(static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
  }
  std::sort(d.begin(), d.end());
  median_pair_ns = d[d.size() / 2];
  min_nonzero_ns = 0;
  for (const std::uint32_t v : d) {
    if (v != 0) {
      min_nonzero_ns = v;
      break;
    }
  }
}

std::uint64_t peak_rss_bytes() {
  struct rusage ru {};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(ru.ru_maxrss);  // bytes
#else
  return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;  // kilobytes
#endif
}

void usage() {
  std::fputs(
      "usage: itch_bench --file FILE --mode frame|decode|book-flat|book-std\n"
      "                  [--runs N] [--label TEXT] [--no-latency]\n",
      stderr);
}

int run(int argc, char** argv) {
  std::string path;
  std::string mode;
  std::string label;
  int runs = 7;
  bool latency = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--file") {
      path = value();
    } else if (arg == "--mode") {
      mode = value();
    } else if (arg == "--runs") {
      runs = std::atoi(value());
    } else if (arg == "--label") {
      label = value();
    } else if (arg == "--no-latency") {
      latency = false;
    } else {
      usage();
      return 2;
    }
  }
  const bool known =
      mode == "frame" || mode == "decode" || mode == "book-flat" || mode == "book-std";
  if (path.empty() || !known || runs < 1) {
    usage();
    return 2;
  }

  // macOS cannot pin a thread to a core. Asking for the highest QoS class is
  // the supported way to keep it on performance cores; record whether it took.
  const char* scheduling = "default";
#if defined(__APPLE__)
  if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0) {
    scheduling = "qos_user_interactive";
  }
#endif

  const itch::MappedFile file(path);
  const std::span<const std::uint8_t> data = file.bytes();

  // Warm-up: fault the file in and warm caches/branch predictors. Its
  // allocation count is the cold-start figure reported below.
  const std::uint64_t allocs_before = g_allocations;
  const std::uint64_t bytes_before = g_allocated_bytes;
  const PassResult warm = run_pass(mode, data, NoHook{});
  const std::uint64_t allocs = g_allocations - allocs_before;
  const std::uint64_t alloc_bytes = g_allocated_bytes - bytes_before;
  // Peak RSS is sampled before the latency pass so its sample buffer is not
  // counted; it still includes the resident pages of the mapped input file.
  const std::uint64_t rss_after_warmup = peak_rss_bytes();

  std::vector<double> seconds;
  seconds.reserve(static_cast<std::size_t>(runs));
  for (int i = 0; i < runs; ++i) {
    const PassResult r = run_pass(mode, data, NoHook{});
    if (r.messages != warm.messages || r.checksum != warm.checksum) {
      std::fprintf(stderr, "itch_bench: non-deterministic pass result\n");
      return 1;
    }
    seconds.push_back(r.seconds);
  }
  const std::uint64_t rss_after_timed = peak_rss_bytes();
  std::vector<double> sorted_seconds = seconds;
  std::sort(sorted_seconds.begin(), sorted_seconds.end());
  const double best = sorted_seconds.front();
  const double median = sorted_seconds[sorted_seconds.size() / 2];
  const auto msgs = static_cast<double>(warm.messages);
  const double mb = static_cast<double>(data.size()) / 1e6;

  std::uint32_t clock_min = 0;
  std::uint32_t clock_pair = 0;
  std::vector<std::uint32_t> samples;
  if (latency) {
    probe_clock(clock_min, clock_pair);
    samples.reserve(static_cast<std::size_t>(warm.messages));
    (void)run_pass(mode, data, LatencyHook{&samples});
    std::sort(samples.begin(), samples.end());
  }

  std::printf("{");
  std::printf("\"label\":\"%s\",", label.c_str());
  std::printf("\"mode\":\"%s\",", mode.c_str());
  std::printf("\"file\":\"%s\",", path.c_str());
  std::printf("\"file_bytes\":%zu,", data.size());
  std::printf("\"messages\":%" PRIu64 ",", warm.messages);
  std::printf("\"runs\":%d,", runs);
  std::printf("\"seconds\":[");
  for (std::size_t i = 0; i < seconds.size(); ++i) {
    std::printf("%s%.6f", i == 0 ? "" : ",", seconds[i]);
  }
  std::printf("],");
  std::printf("\"scheduling\":\"%s\",", scheduling);
  std::printf("\"min_ns_per_msg\":%.2f,", best * 1e9 / msgs);
  std::printf("\"max_ns_per_msg\":%.2f,", sorted_seconds.back() * 1e9 / msgs);
  std::printf("\"median_seconds\":%.6f,", median);
  std::printf("\"best_seconds\":%.6f,", best);
  std::printf("\"median_msgs_per_sec\":%.0f,", msgs / median);
  std::printf("\"best_msgs_per_sec\":%.0f,", msgs / best);
  std::printf("\"median_mb_per_sec\":%.1f,", mb / median);
  std::printf("\"median_ns_per_msg\":%.2f,", median * 1e9 / msgs);
  if (latency) {
    std::printf("\"latency_ns\":{\"p50\":%u,\"p99\":%u,\"p999\":%u,\"max\":%u},",
                percentile(samples, 0.50), percentile(samples, 0.99), percentile(samples, 0.999),
                samples.empty() ? 0 : samples.back());
    std::printf("\"clock_min_nonzero_ns\":%u,", clock_min);
    std::printf("\"clock_read_pair_ns\":%u,", clock_pair);
  }
  std::printf("\"cold_pass_allocations\":%" PRIu64 ",", allocs);
  std::printf("\"cold_pass_allocated_bytes\":%" PRIu64 ",", alloc_bytes);
  std::printf("\"allocations_per_million_msgs\":%.1f,",
              msgs > 0 ? static_cast<double>(allocs) * 1e6 / msgs : 0.0);
  std::printf("\"peak_rss_bytes_after_warmup\":%" PRIu64 ",", rss_after_warmup);
  std::printf("\"peak_rss_bytes_after_timed_runs\":%" PRIu64 ",", rss_after_timed);
  std::printf("\"live_orders_at_end\":%" PRIu64 ",", warm.live_orders);
  std::printf("\"checksum\":\"%016" PRIx64 "\",", warm.checksum);
  std::printf("\"compiler\":\"%s\",", __VERSION__);
  std::printf("\"flags\":\"%s\"", ITCH_BUILD_FLAGS);
  std::printf("}\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "itch_bench: %s\n", e.what());
    return 1;
  }
}
