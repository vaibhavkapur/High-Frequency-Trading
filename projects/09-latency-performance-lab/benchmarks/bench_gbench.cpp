#include <benchmark/benchmark.h>

#include "lpl/allocator_bench.hpp"
#include "lpl/aos_soa.hpp"
#include "lpl/branch_and_sharing.hpp"
#include "lpl/dispatch.hpp"
#include "lpl/order_book_variants.hpp"

namespace {

void BM_AoS_Sequential(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto ds = lpl::make_aos_soa_dataset(n, 42);
  for (auto _ : state) {
    auto v = lpl::scan_sequential_aos(ds.aos);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
}

void BM_SoA_Sequential(benchmark::State& state) {
  const auto n = static_cast<size_t>(state.range(0));
  const auto ds = lpl::make_aos_soa_dataset(n, 42);
  for (auto _ : state) {
    auto v = lpl::scan_sequential_soa(ds.soa);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
}

void BM_OrderBook_Map(benchmark::State& state) {
  const auto depth = static_cast<size_t>(state.range(0));
  const auto evs = lpl::generate_order_book_workload(10000, depth, 1337);
  lpl::MapPriceBook book;
  for (auto _ : state) {
    auto v = lpl::replay_price_book(book, evs);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(evs.size()));
}

void BM_OrderBook_SortedVector(benchmark::State& state) {
  const auto depth = static_cast<size_t>(state.range(0));
  const auto evs = lpl::generate_order_book_workload(10000, depth, 1337);
  lpl::SortedVectorPriceBook book;
  book.reserve(depth * 2);
  for (auto _ : state) {
    auto v = lpl::replay_price_book(book, evs);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(evs.size()));
}

void BM_Allocator_Heap(benchmark::State& state) {
  const auto trace = lpl::generate_alloc_churn_trace(8192, 2048, 2026);
  for (auto _ : state) {
    auto v = lpl::run_heap_churn(trace, 2048);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(trace.size()));
}

void BM_Allocator_ObjectPool(benchmark::State& state) {
  const auto trace = lpl::generate_alloc_churn_trace(8192, 2048, 2026);
  lpl::ObjectPool<lpl::TrackedOrder> pool(2064);
  for (auto _ : state) {
    auto v = lpl::run_pool_churn(pool, trace, 2048);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(trace.size()));
}

void BM_Branch_ForcedUnsorted(benchmark::State& state) {
  const auto ds = lpl::make_branch_dataset(65536, 777);
  for (auto _ : state) {
    auto v = lpl::branch_filter_forced_branch(ds.unsorted, ds.threshold);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * 65536);
}

void BM_Branch_BranchlessUnsorted(benchmark::State& state) {
  const auto ds = lpl::make_branch_dataset(65536, 777);
  for (auto _ : state) {
    auto v = lpl::branch_filter_branchless(ds.unsorted, ds.threshold);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * 65536);
}

BENCHMARK(BM_AoS_Sequential)->Arg(16384)->Arg(131072);
BENCHMARK(BM_SoA_Sequential)->Arg(16384)->Arg(131072);
BENCHMARK(BM_OrderBook_Map)->Arg(64)->Arg(512);
BENCHMARK(BM_OrderBook_SortedVector)->Arg(64)->Arg(512);
BENCHMARK(BM_Allocator_Heap);
BENCHMARK(BM_Allocator_ObjectPool);
BENCHMARK(BM_Branch_ForcedUnsorted);
BENCHMARK(BM_Branch_BranchlessUnsorted);

}  // namespace

BENCHMARK_MAIN();
