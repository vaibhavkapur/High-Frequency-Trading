// One-way producer -> consumer handoff latency.
//
// Method
//   * The producer stamps each message, the consumer reads the clock right
//     after the pop and records (receive - stamp). Both threads read the same
//     process-local monotonic clock (bench::now_ns), so no clock sync is
//     needed.
//   * Paced mode (--rates R, messages/second): the producer busy-waits until
//     each message's *intended* send time start + i/R and stamps the message
//     with that intended time, not the actual one. If the producer or the
//     queue falls behind, the delay shows up in the measurement instead of
//     being silently dropped (avoids coordinated omission).
//   * Unpaced mode (rate 0): the producer pushes as fast as it can and stamps
//     with the actual time just before the first push attempt. The queue runs
//     full, so this measures queueing delay at maximum occupancy.
//   * Queue occupancy is sampled by the consumer every 64th pop.
//   * The first --warmup messages are discarded. All remaining samples are
//     kept in a preallocated array and sorted; percentiles are exact order
//     statistics, not histogram estimates.
//
// Caveats printed with the results: every sample includes one clock read
// (cost reported) and is quantised to the clock step (resolution reported).
//
//   bench_latency [--seconds S] [--rates 100000,1000000,0] [--capacity N]
//                 [--warmup N] [--queues spsc,mpsc,mutex_spin,mutex_cv]
//                 [--max-samples N] [placement flags as bench_throughput]
//                 [--csv FILE]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "common.hpp"
#include "lfm/mpsc_queue.hpp"
#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"

namespace {

struct Msg {
  std::uint64_t seq;
  std::int64_t stamp_ns;
};

template <typename Q>
struct Spinning {
  Q q;
  explicit Spinning(std::size_t cap) : q(cap) {}
  void push(const Msg& m) {
    while (!q.try_push(m)) {
      lfm::cpu_relax();
    }
  }
  void pop(Msg& m) {
    while (!q.try_pop(m)) {
      lfm::cpu_relax();
    }
  }
};

struct Blocking {
  lfm::MutexQueue<Msg> q;
  explicit Blocking(std::size_t cap) : q(cap) {}
  void push(const Msg& m) { q.push(m); }
  void pop(Msg& m) { q.pop(m); }
};

struct Config {
  double seconds;
  std::uint64_t warmup;
  std::uint64_t max_samples;
  std::size_t capacity;
  bench::Placement producer;
  bench::Placement consumer;
  bench::ClockInfo clock;
  std::FILE* csv;
};

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
  const auto rank = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
  return sorted[std::min(rank, sorted.size() - 1)];
}

template <typename Variant>
void run_case(const char* name, std::int64_t rate, const Config& cfg) {
  // Paced: enough messages for --seconds. Unpaced: a fixed count.
  std::uint64_t samples = rate > 0
                              ? static_cast<std::uint64_t>(static_cast<double>(rate) * cfg.seconds)
                              : cfg.max_samples;
  samples = std::min(samples, cfg.max_samples);
  const std::uint64_t total = samples + cfg.warmup;
  const std::int64_t interval_ns = rate > 0 ? 1'000'000'000 / rate : 0;

  Variant v(cfg.capacity);
  std::vector<std::int64_t> latency(samples, 0);  // zero-filled: pages are touched up front
  bench::StartGate gate;
  std::uint64_t occupancy_sum = 0;
  std::uint64_t occupancy_samples = 0;
  bool in_order = true;
  std::string prod_place;
  std::string cons_place;

  std::thread consumer([&] {
    cons_place = cfg.consumer.apply();
    gate.arrive_and_wait();
    Msg m{};
    for (std::uint64_t i = 0; i < total; ++i) {
      v.pop(m);
      const std::int64_t now = bench::now_ns();
      in_order &= m.seq == i;
      if (i >= cfg.warmup) {
        latency[i - cfg.warmup] = now - m.stamp_ns;
        if ((i & 63) == 0) {
          occupancy_sum += v.q.size_approx();
          ++occupancy_samples;
        }
      }
    }
  });
  std::thread producer([&] {
    prod_place = cfg.producer.apply();
    gate.arrive_and_wait();
    const std::int64_t start = bench::now_ns();
    for (std::uint64_t i = 0; i < total; ++i) {
      std::int64_t stamp;
      if (interval_ns > 0) {
        stamp = start + static_cast<std::int64_t>(i) * interval_ns;  // intended send time
        while (bench::now_ns() < stamp) {
          lfm::cpu_relax();
        }
      } else {
        stamp = bench::now_ns();
      }
      v.push(Msg{i, stamp});
    }
  });
  gate.release_when(2);
  producer.join();
  consumer.join();

  if (!in_order) {
    std::fprintf(stderr, "FATAL: %s delivered messages out of order\n", name);
    std::abort();
  }
  std::sort(latency.begin(), latency.end());
  double sum = 0;
  for (const std::int64_t x : latency) {
    sum += static_cast<double>(x);
  }
  const double mean = sum / static_cast<double>(latency.size());
  const double occupancy = occupancy_samples == 0 ? 0.0
                                                  : static_cast<double>(occupancy_sum) /
                                                        static_cast<double>(occupancy_samples);

  std::fprintf(
      cfg.csv, "%s,%lld,%zu,%zu,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%.1f,%.2f,%lld,%.1f,%s,%s\n",
      name, static_cast<long long>(rate), cfg.capacity, latency.size(),
      static_cast<unsigned long long>(cfg.warmup), static_cast<long long>(latency.front()),
      static_cast<long long>(percentile(latency, 0.50)),
      static_cast<long long>(percentile(latency, 0.90)),
      static_cast<long long>(percentile(latency, 0.99)),
      static_cast<long long>(percentile(latency, 0.999)), static_cast<long long>(latency.back()),
      mean, occupancy, static_cast<long long>(cfg.clock.resolution_ns), cfg.clock.cost_ns,
      prod_place.c_str(), cons_place.c_str());
  std::fflush(cfg.csv);
  std::fprintf(
      stderr,
      "  %-10s rate=%-9lld n=%-8zu p50=%-8lld p99=%-8lld p99.9=%-9lld max=%-10lld occ=%.1f\n", name,
      static_cast<long long>(rate), latency.size(),
      static_cast<long long>(percentile(latency, 0.50)),
      static_cast<long long>(percentile(latency, 0.99)),
      static_cast<long long>(percentile(latency, 0.999)), static_cast<long long>(latency.back()),
      occupancy);
}

}  // namespace

int main(int argc, char** argv) {
  const bench::Args args(argc, argv);
  if (args.has("--help")) {
    std::puts("see the comment at the top of benchmarks/bench_latency.cpp");
    return 0;
  }
  Config cfg{};
  cfg.seconds = static_cast<double>(args.get_int("--seconds", 3));
  cfg.warmup = static_cast<std::uint64_t>(args.get_int("--warmup", 100'000));
  cfg.max_samples = static_cast<std::uint64_t>(args.get_int("--max-samples", 5'000'000));
  cfg.capacity = static_cast<std::size_t>(args.get_int("--capacity", 1024));
  cfg.producer.cpu = static_cast<int>(args.get_int("--producer-cpu", -1));
  cfg.consumer.cpu = static_cast<int>(args.get_int("--consumer-cpu", -1));
  cfg.producer.qos = args.get("--producer-qos", "n")[0];
  cfg.consumer.qos = args.get("--consumer-qos", "n")[0];
  const std::string csv_path = args.get("--csv", "");
  cfg.csv = csv_path.empty() ? stdout : std::fopen(csv_path.c_str(), "w");
  if (cfg.csv == nullptr) {
    std::perror(csv_path.c_str());
    return 1;
  }
  const auto rates = args.get_list("--rates", "100000,1000000,0");
  const auto queues = args.get_names("--queues", "spsc,mpsc,mutex_spin,mutex_cv");

  cfg.clock = bench::measure_clock();
  std::fprintf(stderr,
               "bench_latency: clock step %lld ns, clock read cost %.1f ns, capacity %zu, all "
               "latencies in ns\n",
               static_cast<long long>(cfg.clock.resolution_ns), cfg.clock.cost_ns, cfg.capacity);
  std::fprintf(cfg.csv,
               "queue,rate_msgs_per_sec,capacity,samples,warmup,min_ns,p50_ns,p90_ns,p99_ns,"
               "p999_ns,max_ns,mean_ns,mean_occupancy,clock_step_ns,clock_cost_ns,"
               "producer_placement,consumer_placement\n");

  for (const std::int64_t rate : rates) {
    if (bench::contains(queues, "spsc")) {
      run_case<Spinning<lfm::SpscQueue<Msg>>>("spsc", rate, cfg);
    }
    if (bench::contains(queues, "mpsc")) {
      run_case<Spinning<lfm::MpscQueue<Msg>>>("mpsc", rate, cfg);
    }
    if (bench::contains(queues, "mutex_spin")) {
      run_case<Spinning<lfm::MutexQueue<Msg>>>("mutex_spin", rate, cfg);
    }
    if (bench::contains(queues, "mutex_cv")) {
      run_case<Blocking>("mutex_cv", rate, cfg);
    }
  }
  if (cfg.csv != stdout) {
    std::fclose(cfg.csv);
  }
  return 0;
}
