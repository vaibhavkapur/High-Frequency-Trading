// Google Benchmark microbenchmarks: single-threaded cost of the queue
// operations themselves, with no second thread and therefore no cache-line
// transfer. This isolates the instruction-path cost (index math, atomics,
// lock/unlock) from the cross-core effects measured by bench_throughput and
// bench_latency. Uncontended numbers flatter the mutex queue: an uncontended
// lock is cheap, a contended one is not.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <benchmark/benchmark.h>

#include "lfm/mpsc_queue.hpp"
#include "lfm/mutex_queue.hpp"
#include "lfm/spsc_queue.hpp"

namespace {

struct Msg {
  std::uint64_t seq;
  std::uint64_t payload[7];
};
static_assert(sizeof(Msg) == 64);

template <typename Queue>
void BM_PushPop(benchmark::State& state) {
  Queue q(1024);
  Msg in{};
  Msg out{};
  for (auto _ : state) {
    ++in.seq;
    benchmark::DoNotOptimize(q.try_push(in));
    benchmark::DoNotOptimize(q.try_pop(out));
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PushPop<lfm::SpscQueue<Msg>>)->Name("PushPop/spsc");
BENCHMARK(BM_PushPop<lfm::MpscQueue<Msg>>)->Name("PushPop/mpsc");
BENCHMARK(BM_PushPop<lfm::MutexQueue<Msg>>)->Name("PushPop/mutex");

// Batch of N through the queue: one cursor publication per batch instead of
// one per message.
void BM_SpscBatch(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  lfm::SpscQueue<Msg> q(1024);
  std::array<Msg, 256> in{};
  std::array<Msg, 256> out{};
  for (auto _ : state) {
    benchmark::DoNotOptimize(q.try_push_batch(std::span<const Msg>(in.data(), n)));
    benchmark::DoNotOptimize(q.try_pop_batch(std::span<Msg>(out.data(), n)));
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_SpscBatch)->Name("BatchPushPop/spsc")->RangeMultiplier(4)->Range(1, 256);

// Same N messages, one at a time, for comparison with the batch API.
void BM_SpscOneByOne(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  lfm::SpscQueue<Msg> q(1024);
  std::array<Msg, 256> in{};
  std::array<Msg, 256> out{};
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) {
      benchmark::DoNotOptimize(q.try_push(in[i]));
    }
    for (std::size_t i = 0; i < n; ++i) {
      benchmark::DoNotOptimize(q.try_pop(out[i]));
    }
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_SpscOneByOne)->Name("OneByOnePushPop/spsc")->RangeMultiplier(4)->Range(1, 256);

}  // namespace

BENCHMARK_MAIN();
