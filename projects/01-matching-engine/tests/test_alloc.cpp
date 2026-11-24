// Allocation tests. This binary links me_alloc_counter, which replaces the global
// operator new, so heap allocations made while processing commands can be counted.
#include "me/alloc_counter.hpp"
#include "me/workload.hpp"
#include "test_util.hpp"

namespace me::test {
namespace {

struct Counts {
  std::uint64_t allocations = 0;
  std::uint64_t reports = 0;
  EngineStats stats;
};

template <class Book>
Counts measure(const WorkloadConfig& wc, std::size_t warmup, std::size_t measured) {
  Workload workload(wc);
  const std::vector<Command> commands = workload.generate(warmup + measured);
  Engine<Book> engine(make_config(wc.symbols, wc.tick_size, wc.lot_size, 1u << 16));
  Counts counts;
  const auto sink = [&counts](const Report&) { ++counts.reports; };
  for (std::size_t i = 0; i < warmup; ++i) engine.process(commands[i], sink);

  const std::uint64_t before = alloc_count();
  for (std::size_t i = warmup; i < warmup + measured; ++i) engine.process(commands[i], sink);
  counts.allocations = alloc_count() - before;
  counts.stats = engine.stats();
  return counts;
}

WorkloadConfig mixed_workload() {
  WorkloadConfig wc;
  wc.symbols = 4;
  wc.target_live = 5000;
  wc.depth_levels = 200;
  wc.invalid_pm = 20;
  wc.random_ids = true;
  return wc;
}

// Escapes a pointer so the compiler cannot elide the allocation behind it.
void* volatile g_escape = nullptr;

TEST(Allocation, CounterSeesHeapAllocations) {
  const std::uint64_t before = alloc_count();
  auto* p = new int(7);
  g_escape = p;
  const std::uint64_t after = alloc_count();
  delete static_cast<int*>(g_escape);
  EXPECT_EQ(after - before, 1u);
}

TEST(Allocation, PoolBookProcessesCommandsWithoutAllocating) {
  const Counts counts = measure<PoolBook>(mixed_workload(), 50'000, 200'000);
  EXPECT_EQ(counts.allocations, 0u);
  // The measured window must have done real work of every kind.
  EXPECT_GT(counts.stats.trades, 1000u);
  EXPECT_GT(counts.stats.canceled, 1000u);
  EXPECT_GT(counts.stats.rejected, 1000u);
  EXPECT_GT(counts.stats.reduced, 1000u);
  EXPECT_GT(counts.stats.replaced, 1000u);
}

TEST(Allocation, PoolBookDoesNotAllocateEvenFromAColdStart) {
  // No warm-up: everything is sized in the constructor (levels stay below the reserved 4096).
  const Counts counts = measure<PoolBook>(mixed_workload(), 0, 100'000);
  EXPECT_EQ(counts.allocations, 0u);
}

TEST(Allocation, MapBookBaselineAllocatesOnTheHotPath) {
  // Documents the tradeoff PoolBook exists to remove; not a requirement on MapBook.
  const Counts counts = measure<MapBook>(mixed_workload(), 50'000, 200'000);
  EXPECT_GT(counts.allocations, 100'000u);
}

}  // namespace
}  // namespace me::test
