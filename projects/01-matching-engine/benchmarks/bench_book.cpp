// Google Benchmark microbenchmarks: one operation kind at a time, against a book
// pre-filled to a given depth, for both book implementations.
//
// Each benchmark iteration times a batch of kBatch engine commands and then
// restores the book outside the timed region, so the pause/resume overhead is
// amortised over the batch. Read "items_per_second" (or divide real_time by
// kBatch) for the per-command cost. Costs include command validation and report
// construction, with a sink that only counts reports.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"

namespace {

using me::Command;
using me::CommandType;
using me::OrderId;
using me::Price;
using me::Side;

constexpr std::size_t kBatch = 1024;
constexpr Price kMid = 1'000'000;

std::uint64_t splitmix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

Command make_new(OrderId id, Side side, Price price, me::Qty qty) {
  Command c;
  c.type = CommandType::New;
  c.order_id = id;
  c.side = side;
  c.price = price;
  c.qty = qty;
  return c;
}

Command make_cancel(OrderId id) {
  Command c;
  c.type = CommandType::Cancel;
  c.order_id = id;
  return c;
}

struct CountingSink {
  std::uint64_t reports = 0;
  void operator()(const me::Report& r) {
    ++reports;
    me::Seq seq = r.seq;
    benchmark::DoNotOptimize(seq);
  }
};

// A book holding `depth` resting orders, about 16 per price level, half on each side.
template <class Book>
struct Fixture {
  explicit Fixture(std::size_t depth) : levels(static_cast<Price>(depth / 32 + 1)), engine(config(depth)), next_id(1) {
    std::uint64_t rng = 42;
    for (std::size_t i = 0; i < depth; ++i) {
      const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
      engine.process(make_new(next_id++, side, passive_price(side, rng), 10), sink);
    }
  }

  static me::EngineConfig config(std::size_t depth) {
    me::EngineConfig c;
    c.symbols.resize(1);
    c.max_orders_per_book = static_cast<std::uint32_t>(depth + 4 * kBatch);
    return c;
  }

  // Bids at or below kMid-9, asks at or above kMid+9: the prices in between are
  // left free for the match and sweep benchmarks to place their own orders.
  Price passive_price(Side side, std::uint64_t& rng) const {
    const Price offset = 9 + static_cast<Price>(splitmix(rng) % static_cast<std::uint64_t>(levels));
    return side == Side::Buy ? kMid - offset : kMid + offset;
  }

  Price levels;
  CountingSink sink;
  me::Engine<Book> engine;
  OrderId next_id;
};

template <class Book>
void BM_AddPassive(benchmark::State& state) {
  Fixture<Book> f(static_cast<std::size_t>(state.range(0)));
  std::vector<Command> adds;
  std::vector<Command> cancels;
  std::uint64_t rng = 7;
  for (std::size_t i = 0; i < kBatch; ++i) {
    const Side side = splitmix(rng) % 2 == 0 ? Side::Buy : Side::Sell;
    adds.push_back(make_new(f.next_id, side, f.passive_price(side, rng), 10));
    cancels.push_back(make_cancel(f.next_id++));
  }
  for (auto _ : state) {
    for (const Command& c : adds) f.engine.process(c, f.sink);
    state.PauseTiming();
    for (const Command& c : cancels) f.engine.process(c, f.sink);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

template <class Book>
void BM_Cancel(benchmark::State& state) {
  Fixture<Book> f(static_cast<std::size_t>(state.range(0)));
  std::vector<Command> adds;
  std::vector<Command> cancels;
  std::uint64_t rng = 7;
  for (std::size_t i = 0; i < kBatch; ++i) {
    const Side side = splitmix(rng) % 2 == 0 ? Side::Buy : Side::Sell;
    adds.push_back(make_new(f.next_id, side, f.passive_price(side, rng), 10));
    cancels.push_back(make_cancel(f.next_id++));
  }
  for (std::size_t i = kBatch - 1; i > 0; --i) {  // cancel in an order unrelated to arrival
    std::swap(cancels[i], cancels[splitmix(rng) % (i + 1)]);
  }
  for (auto _ : state) {
    state.PauseTiming();
    for (const Command& c : adds) f.engine.process(c, f.sink);
    state.ResumeTiming();
    for (const Command& c : cancels) f.engine.process(c, f.sink);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

// Each timed command is a marketable buy that fully fills exactly one resting sell.
template <class Book>
void BM_AggressiveMatch(benchmark::State& state) {
  Fixture<Book> f(static_cast<std::size_t>(state.range(0)));
  std::vector<Command> makers;
  std::vector<Command> takers;
  for (std::size_t i = 0; i < kBatch; ++i) {
    makers.push_back(make_new(f.next_id++, Side::Sell, kMid, 10));
    takers.push_back(make_new(f.next_id++, Side::Buy, kMid, 10));
  }
  for (auto _ : state) {
    state.PauseTiming();
    for (const Command& c : makers) f.engine.process(c, f.sink);
    state.ResumeTiming();
    for (const Command& c : takers) f.engine.process(c, f.sink);
  }
  if (f.engine.stats().trades != static_cast<std::uint64_t>(state.iterations()) * kBatch) {
    state.SkipWithError("each taker should have produced exactly one trade");
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

// Each timed command is a marketable buy that sweeps `range(1)` resting sells
// spread over 8 price levels. Items processed = fills, so items_per_second is fills/s.
template <class Book>
void BM_Sweep(benchmark::State& state) {
  Fixture<Book> f(static_cast<std::size_t>(state.range(0)));
  const auto fills = static_cast<std::size_t>(state.range(1));
  const std::size_t takers_per_batch = std::max<std::size_t>(1, kBatch / fills);
  std::vector<Command> makers;
  std::vector<Command> takers;
  for (std::size_t i = 0; i < takers_per_batch * fills; ++i) {
    makers.push_back(make_new(f.next_id++, Side::Sell, kMid + static_cast<Price>(i % 8), 10));
  }
  for (std::size_t i = 0; i < takers_per_batch; ++i) {
    takers.push_back(make_new(f.next_id++, Side::Buy, kMid + 7, static_cast<me::Qty>(10 * fills)));
  }
  for (auto _ : state) {
    state.PauseTiming();
    for (const Command& c : makers) f.engine.process(c, f.sink);
    state.ResumeTiming();
    for (const Command& c : takers) f.engine.process(c, f.sink);
  }
  if (f.engine.stats().trades != static_cast<std::uint64_t>(state.iterations()) * takers_per_batch * fills) {
    state.SkipWithError("each taker should have produced exactly `fills` trades");
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(takers_per_batch * fills));
}

#define ME_BENCH(fn)                                                                          \
  BENCHMARK_TEMPLATE(fn, me::MapBook)->Arg(1'000)->Arg(10'000)->Arg(100'000)->Arg(1'000'000); \
  BENCHMARK_TEMPLATE(fn, me::PoolBook)->Arg(1'000)->Arg(10'000)->Arg(100'000)->Arg(1'000'000)

ME_BENCH(BM_AddPassive);
ME_BENCH(BM_Cancel);
ME_BENCH(BM_AggressiveMatch);
BENCHMARK_TEMPLATE(BM_Sweep, me::MapBook)->Args({10'000, 8})->Args({10'000, 64})->Args({10'000, 512});
BENCHMARK_TEMPLATE(BM_Sweep, me::PoolBook)->Args({10'000, 8})->Args({10'000, 64})->Args({10'000, 512});

}  // namespace

BENCHMARK_MAIN();
