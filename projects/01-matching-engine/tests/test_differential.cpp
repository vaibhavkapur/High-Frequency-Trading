// Randomized differential tests: both production books must agree with the naive
// reference engine on every report, and keep their structural invariants, over
// long generated command streams that include malformed input.
#include <tuple>

#include "me/workload.hpp"
#include "reference_engine.hpp"
#include "test_util.hpp"

namespace me::test {
namespace {

struct Scenario {
  const char* name;
  WorkloadConfig workload;
  std::uint32_t max_orders;  // per book
  std::size_t commands;
};

Scenario scenario(const char* name, std::size_t commands, std::uint32_t max_orders, void (*tweak)(WorkloadConfig&)) {
  WorkloadConfig w;
  w.tick_size = 5;
  w.lot_size = 10;
  w.mid_ticks = 1000;
  w.depth_levels = 20;
  w.target_live = 150;
  w.invalid_pm = 60;
  tweak(w);
  return Scenario{name, w, max_orders, commands};
}

const Scenario kScenarios[] = {
    scenario("balanced", 20000, 4096, [](WorkloadConfig&) {}),
    scenario("multi_symbol", 20000, 4096, [](WorkloadConfig& w) { w.symbols = 5; }),
    scenario("random_ids", 20000, 4096, [](WorkloadConfig& w) { w.random_ids = true; }),
    scenario("aggressive", 20000, 4096,
             [](WorkloadConfig& w) {
               w.aggressive_pm = 500;
               w.max_lots = 300;
             }),
    scenario("deep_sparse_book", 20000, 4096,
             [](WorkloadConfig& w) {
               w.depth_levels = 400;
               w.target_live = 300;
             }),
    scenario("one_price_level", 15000, 4096,
             [](WorkloadConfig& w) {
               w.depth_levels = 1;
               w.aggressive_pm = 200;
             }),
    // Tiny bound: exercises BookFull and a nearly full pool / hash index.
    scenario("book_full", 20000, 24,
             [](WorkloadConfig& w) {
               w.target_live = 60;
               w.random_ids = true;
             }),
    scenario("modify_heavy", 20000, 4096,
             [](WorkloadConfig& w) {
               w.add_pm = 400;
               w.cancel_pm = 200;
               w.reduce_pm = 200;
               w.replace_pm = 200;
             }),
};

class Differential : public ::testing::TestWithParam<std::tuple<Scenario, std::uint64_t>> {};

template <class Book>
void expect_same_depth(const Engine<Book>& engine, const ReferenceEngine& reference, SymbolId symbol,
                       std::size_t step) {
  for (Side side : {Side::Buy, Side::Sell}) {
    const std::vector<LevelView> expected = reference.depth(symbol, side);
    ASSERT_EQ(engine.depth(symbol, side, expected.size() + 1), expected)
        << Book::name() << " depth mismatch at command " << step << " symbol " << symbol;
    const auto best = engine.book(symbol).best(side);
    ASSERT_EQ(best.has_value(), !expected.empty());
    if (best) ASSERT_EQ(*best, expected.front());
  }
  ASSERT_EQ(engine.book(symbol).size(), reference.resting(symbol));
}

TEST_P(Differential, ProductionBooksMatchReferenceEngine) {
  Scenario sc = std::get<0>(GetParam());
  sc.workload.seed = std::get<1>(GetParam());

  EngineConfig config = make_config(sc.workload.symbols, sc.workload.tick_size, sc.workload.lot_size, sc.max_orders);
  Engine<MapBook> map_engine(config);
  Engine<PoolBook> pool_engine(config);
  ReferenceEngine reference(config);
  Workload workload(sc.workload);

  std::vector<Report> map_out;
  std::vector<Report> pool_out;
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;
  for (std::size_t step = 0; step < sc.commands; ++step) {
    const Command c = workload.next();
    const std::vector<Report> expected = reference.process(c);
    map_out.clear();
    pool_out.clear();
    map_engine.process(c, [&map_out](const Report& r) { map_out.push_back(r); });
    pool_engine.process(c, [&pool_out](const Report& r) { pool_out.push_back(r); });
    ASSERT_EQ(map_out, expected) << "MapBook diverged at command " << step << ": " << format_command(c);
    ASSERT_EQ(pool_out, expected) << "PoolBook diverged at command " << step << ": " << format_command(c);
    for (const Report& r : expected) {
      trades += r.type == ReportType::Trade ? 1 : 0;
      rejects += r.type == ReportType::Rejected ? 1 : 0;
    }

    if (step % 64 == 0 || step + 1 == sc.commands) {
      for (SymbolId s = 0; s < config.symbols.size(); ++s) {
        std::string why;
        ASSERT_TRUE(map_engine.book(s).check_invariants(&why)) << "MapBook at command " << step << ": " << why;
        ASSERT_TRUE(pool_engine.book(s).check_invariants(&why)) << "PoolBook at command " << step << ": " << why;
        ASSERT_NO_FATAL_FAILURE(expect_same_depth(map_engine, reference, s, step));
        ASSERT_NO_FATAL_FAILURE(expect_same_depth(pool_engine, reference, s, step));
      }
    }
  }
  EXPECT_EQ(map_engine.stats(), pool_engine.stats());
  // Guard against a degenerate workload that would make the comparison vacuous.
  EXPECT_GT(trades, sc.commands / 100) << "workload produced almost no trades";
  EXPECT_GT(rejects, sc.commands / 100) << "workload produced almost no rejects";
  EXPECT_GT(map_engine.stats().canceled, sc.commands / 100);
  EXPECT_GT(map_engine.stats().reduced + map_engine.stats().replaced, sc.commands / 200);
}

INSTANTIATE_TEST_SUITE_P(Workloads, Differential,
                         ::testing::Combine(::testing::ValuesIn(kScenarios), ::testing::Values(1u, 2u, 3u, 0xC0FFEEu)),
                         [](const ::testing::TestParamInfo<Differential::ParamType>& info) {
                           return std::string(std::get<0>(info.param).name) + "_seed" +
                                  std::to_string(std::get<1>(info.param));
                         });

}  // namespace
}  // namespace me::test
