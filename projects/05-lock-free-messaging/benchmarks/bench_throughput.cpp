// Two-thread throughput benchmark: one producer, one consumer, N messages.
//
// Sweeps queue implementation x message size x capacity and writes one CSV
// row per run (no aggregation here; scripts/summarize.py does that so the raw
// rows stay available).
//
//   bench_throughput [--messages N] [--runs R] [--sizes 8,64,256]
//                    [--capacities 64,1024,65536] [--queues a,b,...]
//                    [--producer-cpu C --consumer-cpu C]   (Linux hard pin)
//                    [--producer-qos p|e --consumer-qos p|e] (macOS hint)
//                    [--csv FILE]
//
// Queue variants:
//   spsc           lfm::SpscQueue, try_push/try_pop, busy-spin on full/empty
//   spsc_unpadded  same code with cursors packed on one cache line
//                  (CursorAlign=8) to expose false sharing
//   spsc_batch     lfm::SpscQueue via try_push_batch(32) / consume()
//   mpsc           lfm::MpscQueue with a single producer
//   mutex_spin     lfm::MutexQueue via try_push/try_pop, busy-spin
//   mutex_cv       lfm::MutexQueue via blocking push/pop (condition variables)
//   deque_mutex    std::deque + std::mutex, bounded by a size check, busy-spin
//
// Every consumer verifies that sequence numbers arrive in order, so a run
// doubles as a correctness check; a failed check aborts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>

#include "common.hpp"
#include "lfm/mpsc_queue.hpp"
#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"

namespace {

template <std::size_t N>
struct Msg {
  std::uint64_t seq;
  unsigned char pad[N - 8];
};
template <>
struct Msg<8> {
  std::uint64_t seq;
};
static_assert(sizeof(Msg<8>) == 8 && sizeof(Msg<64>) == 64 && sizeof(Msg<256>) == 256);

struct Side {
  std::uint64_t retries = 0;  // failed try_push / try_pop attempts (spin iterations)
  std::int64_t cpu_ns = 0;
  std::string placement;
  bool in_order = true;
};

template <typename Q, typename T>
void spin_produce(Q& q, std::uint64_t n, Side& s) {
  T m{};
  for (std::uint64_t i = 0; i < n; ++i) {
    m.seq = i;
    while (!q.try_push(m)) {
      ++s.retries;
      lfm::cpu_relax();
    }
  }
}

template <typename Q, typename T>
void spin_consume(Q& q, std::uint64_t n, Side& s) {
  T m{};
  for (std::uint64_t i = 0; i < n; ++i) {
    while (!q.try_pop(m)) {
      ++s.retries;
      lfm::cpu_relax();
    }
    s.in_order &= m.seq == i;
  }
}

template <typename T, std::size_t Align>
struct SpscVariant {
  lfm::SpscQueue<T, Align> q;
  explicit SpscVariant(std::size_t cap) : q(cap) {}
  void produce(std::uint64_t n, Side& s) { spin_produce<decltype(q), T>(q, n, s); }
  void consume(std::uint64_t n, Side& s) { spin_consume<decltype(q), T>(q, n, s); }
};
template <typename T>
using Spsc = SpscVariant<T, lfm::kCacheLine>;
template <typename T>
using SpscUnpadded = SpscVariant<T, 8>;

template <typename T>
struct SpscBatch {
  static constexpr std::size_t kBatch = 32;
  lfm::SpscQueue<T> q;
  explicit SpscBatch(std::size_t cap) : q(cap) {}

  void produce(std::uint64_t n, Side& s) {
    std::array<T, kBatch> batch{};
    std::uint64_t next = 0;
    while (next < n) {
      const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kBatch, n - next));
      for (std::size_t i = 0; i < want; ++i) {
        batch[i].seq = next + i;
      }
      std::size_t sent = 0;
      while (sent < want) {
        const std::size_t k =
            q.try_push_batch(std::span<const T>(batch).subspan(sent, want - sent));
        if (k == 0) {
          ++s.retries;
          lfm::cpu_relax();
        }
        sent += k;
      }
      next += want;
    }
  }

  void consume(std::uint64_t n, Side& s) {
    std::uint64_t expect = 0;
    while (expect < n) {
      const std::size_t got = q.consume([&](T&& m) { s.in_order &= m.seq == expect++; });
      if (got == 0) {
        ++s.retries;
        lfm::cpu_relax();
      }
    }
  }
};

template <typename T>
struct Mpsc {
  lfm::MpscQueue<T> q;
  explicit Mpsc(std::size_t cap) : q(cap) {}
  void produce(std::uint64_t n, Side& s) { spin_produce<decltype(q), T>(q, n, s); }
  void consume(std::uint64_t n, Side& s) { spin_consume<decltype(q), T>(q, n, s); }
};

template <typename T>
struct MutexSpin {
  lfm::MutexQueue<T> q;
  explicit MutexSpin(std::size_t cap) : q(cap) {}
  void produce(std::uint64_t n, Side& s) { spin_produce<decltype(q), T>(q, n, s); }
  void consume(std::uint64_t n, Side& s) { spin_consume<decltype(q), T>(q, n, s); }
};

template <typename T>
struct MutexCv {
  lfm::MutexQueue<T> q;
  explicit MutexCv(std::size_t cap) : q(cap) {}
  void produce(std::uint64_t n, Side&) {
    T m{};
    for (std::uint64_t i = 0; i < n; ++i) {
      m.seq = i;
      q.push(m);
    }
  }
  void consume(std::uint64_t n, Side& s) {
    T m{};
    for (std::uint64_t i = 0; i < n; ++i) {
      q.pop(m);
      s.in_order &= m.seq == i;
    }
  }
};

// "Standard container" baseline: what you get by wrapping std::deque in a lock.
template <typename T>
struct DequeMutex {
  struct Queue {
    std::mutex mu;
    std::deque<T> items;
    std::size_t cap;
    bool try_push(const T& v) {
      std::lock_guard lock(mu);
      if (items.size() == cap) {
        return false;
      }
      items.push_back(v);
      return true;
    }
    bool try_pop(T& out) {
      std::lock_guard lock(mu);
      if (items.empty()) {
        return false;
      }
      out = items.front();
      items.pop_front();
      return true;
    }
  } q;
  explicit DequeMutex(std::size_t cap) { q.cap = cap; }
  void produce(std::uint64_t n, Side& s) { spin_produce<Queue, T>(q, n, s); }
  void consume(std::uint64_t n, Side& s) { spin_consume<Queue, T>(q, n, s); }
};

struct Config {
  std::uint64_t messages;
  int runs;
  bench::Placement producer;
  bench::Placement consumer;
  std::FILE* csv;
};

template <typename Variant>
void run_case(const char* name, std::size_t msg_bytes, std::size_t capacity, const Config& cfg) {
  for (int run = 0; run < cfg.runs; ++run) {
    Variant v(capacity);
    bench::StartGate gate;
    Side prod;
    Side cons;
    std::int64_t t_end = 0;

    std::thread consumer([&] {
      cons.placement = cfg.consumer.apply();
      gate.arrive_and_wait();
      const std::int64_t cpu0 = bench::thread_cpu_ns();
      v.consume(cfg.messages, cons);
      t_end = bench::now_ns();
      cons.cpu_ns = bench::thread_cpu_ns() - cpu0;
    });
    std::thread producer([&] {
      prod.placement = cfg.producer.apply();
      gate.arrive_and_wait();
      const std::int64_t cpu0 = bench::thread_cpu_ns();
      v.produce(cfg.messages, prod);
      prod.cpu_ns = bench::thread_cpu_ns() - cpu0;
    });
    while (gate.ready.load() != 2) {
      std::this_thread::yield();
    }
    const std::int64_t t_start = bench::now_ns();
    gate.go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    if (!cons.in_order) {
      std::fprintf(stderr, "FATAL: %s delivered messages out of order\n", name);
      std::abort();
    }
    const std::int64_t elapsed = t_end - t_start;
    const double rate = static_cast<double>(cfg.messages) * 1e9 / static_cast<double>(elapsed);
    std::fprintf(
        cfg.csv, "%s,%zu,%zu,%llu,%d,%lld,%.0f,%llu,%llu,%lld,%lld,%s,%s\n", name, msg_bytes,
        capacity, static_cast<unsigned long long>(cfg.messages), run,
        static_cast<long long>(elapsed), rate, static_cast<unsigned long long>(prod.retries),
        static_cast<unsigned long long>(cons.retries), static_cast<long long>(prod.cpu_ns),
        static_cast<long long>(cons.cpu_ns), prod.placement.c_str(), cons.placement.c_str());
    std::fflush(cfg.csv);
    std::fprintf(stderr, "  %-14s msg=%-4zu cap=%-6zu run=%d  %8.2f M msgs/s\n", name, msg_bytes,
                 capacity, run, rate / 1e6);
  }
}

template <template <typename> class Variant>
void run_variant(const char* name, const std::vector<std::int64_t>& sizes,
                 const std::vector<std::int64_t>& capacities, const Config& cfg) {
  for (const std::int64_t size : sizes) {
    for (const std::int64_t cap64 : capacities) {
      const auto cap = static_cast<std::size_t>(cap64);
      switch (size) {
        case 8:
          run_case<Variant<Msg<8>>>(name, 8, cap, cfg);
          break;
        case 64:
          run_case<Variant<Msg<64>>>(name, 64, cap, cfg);
          break;
        case 256:
          run_case<Variant<Msg<256>>>(name, 256, cap, cfg);
          break;
        default:
          std::fprintf(stderr, "unsupported message size %lld (use 8, 64 or 256)\n",
                       static_cast<long long>(size));
          std::exit(2);
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const bench::Args args(argc, argv);
  if (args.has("--help")) {
    std::puts("see the comment at the top of benchmarks/bench_throughput.cpp");
    return 0;
  }
  Config cfg{};
  cfg.messages = static_cast<std::uint64_t>(args.get_int("--messages", 4'000'000));
  cfg.runs = static_cast<int>(args.get_int("--runs", 3));
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
  const auto sizes = args.get_list("--sizes", "8,64,256");
  const auto capacities = args.get_list("--capacities", "64,1024,65536");
  const auto queues = args.get_names(
      "--queues", "spsc,spsc_unpadded,spsc_batch,mpsc,mutex_spin,mutex_cv,deque_mutex");

  std::fprintf(stderr, "bench_throughput: %llu messages/run, %d runs, affinity support: %s\n",
               static_cast<unsigned long long>(cfg.messages), cfg.runs,
               lfm::affinity_support().c_str());
  std::fprintf(cfg.csv,
               "queue,msg_bytes,capacity,messages,run,elapsed_ns,msgs_per_sec,producer_retries,"
               "consumer_retries,producer_cpu_ns,consumer_cpu_ns,producer_placement,"
               "consumer_placement\n");

  if (bench::contains(queues, "spsc")) run_variant<Spsc>("spsc", sizes, capacities, cfg);
  if (bench::contains(queues, "spsc_unpadded")) {
    run_variant<SpscUnpadded>("spsc_unpadded", sizes, capacities, cfg);
  }
  if (bench::contains(queues, "spsc_batch")) {
    run_variant<SpscBatch>("spsc_batch", sizes, capacities, cfg);
  }
  if (bench::contains(queues, "mpsc")) run_variant<Mpsc>("mpsc", sizes, capacities, cfg);
  if (bench::contains(queues, "mutex_spin")) {
    run_variant<MutexSpin>("mutex_spin", sizes, capacities, cfg);
  }
  if (bench::contains(queues, "mutex_cv")) run_variant<MutexCv>("mutex_cv", sizes, capacities, cfg);
  if (bench::contains(queues, "deque_mutex")) {
    run_variant<DequeMutex>("deque_mutex", sizes, capacities, cfg);
  }

  if (cfg.csv != stdout) {
    std::fclose(cfg.csv);
  }
  return 0;
}
