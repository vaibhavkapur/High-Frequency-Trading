#include <cstdlib>
#include <iostream>
#include <string>

#include "numa_engine/event_generator.hpp"
#include "numa_engine/matching_engine.hpp"
#include "numa_engine/pipeline.hpp"
#include "numa_engine/topology.hpp"

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                              \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #cond "\n";  \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define EXPECT_EQ(a, b)                                                                \
    do {                                                                               \
        const auto _va = (a);                                                          \
        const auto _vb = (b);                                                          \
        if (_va != _vb) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": "              \
                      << #a << " (" << _va << ") != " << #b << " (" << _vb << ")\n";   \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

std::string fixture_path(const std::string& name) {
    return std::string(NUMA_ENGINE_FIXTURE_DIR) + "/" + name;
}

void test_valid_small_fixture_against_independent_expected_result() {
    auto ev_res = numa_engine::load_events_csv(fixture_path("valid_small_events.csv"));
    EXPECT_TRUE(ev_res.ok());
    if (!ev_res.ok()) return;

    const auto& events = ev_res.value();
    EXPECT_EQ(events.size(), 12U);

    // Run directly through isolated domain MatchingEngine (no I/O or threading)
    numa_engine::MatchingEngine engine;
    for (const auto& ev : events) {
        (void)engine.process(ev);
    }

    const auto s = engine.summary();
    // Matches fixtures/expected_small_output.json independently derived trace
    EXPECT_EQ(s.total_events, 12ULL);
    EXPECT_EQ(s.applied_events, 10ULL);
    EXPECT_EQ(s.noop_events, 2ULL);
    EXPECT_EQ(s.add_events, 5ULL);
    EXPECT_EQ(s.modify_events, 1ULL);
    EXPECT_EQ(s.cancel_events, 3ULL);
    EXPECT_EQ(s.trade_events, 3ULL);
    EXPECT_EQ(s.last_seq_id, 12ULL);
    EXPECT_EQ(s.total_volume, 220ULL);
    EXPECT_EQ(s.total_notional_ticks, 280001800LL);
    EXPECT_TRUE(s.sequence_strictly_monotonic);

    const auto& b0 = engine.book(0);
    EXPECT_EQ(b0.best_bid_ticks, 1000005LL);
    EXPECT_EQ(b0.bid_qty, 30ULL);
    EXPECT_EQ(b0.best_ask_ticks, 1000020LL);
    EXPECT_EQ(b0.ask_qty, 110ULL);
    EXPECT_EQ(b0.cumulative_volume, 40ULL);

    const auto& b1 = engine.book(1);
    EXPECT_EQ(b1.best_bid_ticks, 2000010LL);
    EXPECT_EQ(b1.bid_qty, 150ULL);
    EXPECT_EQ(b1.best_ask_ticks, 0LL);
    EXPECT_EQ(b1.ask_qty, 0ULL);
    EXPECT_EQ(b1.cumulative_volume, 100ULL);

    const auto& b2 = engine.book(2);
    EXPECT_EQ(b2.best_bid_ticks, 0LL);
    EXPECT_EQ(b2.bid_qty, 0ULL);
    EXPECT_EQ(b2.best_ask_ticks, 0LL);
    EXPECT_EQ(b2.ask_qty, 0ULL);
    EXPECT_EQ(b2.cumulative_volume, 80ULL);

    // Also verify running the same fixture through both pipelines matches exactly
    const auto topo = numa_engine::detect_topology();
    numa_engine::PipelineConfig cfg;
    cfg.scenario_name = "fixture_spsc";
    cfg.pipeline_kind = numa_engine::PipelineKind::kLockFreeSpsc;
    cfg.memory_policy = numa_engine::MemoryPolicy::kFirstTouchLocal;
    cfg.queue_capacity = 64;
    auto run_res = numa_engine::run_pipeline(cfg, events, topo);
    EXPECT_TRUE(run_res.ok());
    if (run_res.ok()) {
        EXPECT_TRUE(run_res.value().engine_summary == s);
        EXPECT_EQ(run_res.value().lost_events, 0ULL);
    }
}

void test_boundary_fixture() {
    auto ev_res = numa_engine::load_events_csv(fixture_path("boundary_events.csv"));
    EXPECT_TRUE(ev_res.ok());
    if (!ev_res.ok()) return;

    EXPECT_EQ(ev_res.value().size(), 6U);
    numa_engine::MatchingEngine engine;
    for (const auto& ev : ev_res.value()) {
        (void)engine.process(ev);
    }
    const auto s = engine.summary();
    EXPECT_EQ(s.total_events, 6ULL);
    EXPECT_EQ(s.applied_events, 5ULL);
    EXPECT_EQ(s.noop_events, 1ULL);  // Event 4 cancels already-empty bid side
    EXPECT_EQ(engine.book(63).bid_qty, 0ULL);
    EXPECT_EQ(engine.book(63).ask_qty, 429496729ULL);
    EXPECT_EQ(engine.book(0).bid_qty, 0ULL);
    EXPECT_EQ(engine.book(0).cumulative_volume, 1000ULL);
}

void test_invalid_fixtures_rejected_with_structured_errors() {
    {
        auto r = numa_engine::load_events_csv(fixture_path("invalid_negative_price.csv"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kInvalidPrice));
        EXPECT_EQ(r.error().field, "price_ticks");
    }
    {
        auto r = numa_engine::load_events_csv(fixture_path("invalid_non_monotonic_seq.csv"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kNonMonotonicSequence));
        EXPECT_EQ(r.error().field, "seq_id");
    }
    {
        auto r = numa_engine::load_events_csv(fixture_path("invalid_unknown_type.csv"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kInvalidEventType));
        EXPECT_EQ(r.error().field, "event_type");
    }
    {
        auto r = numa_engine::load_events_csv(fixture_path("does_not_exist.csv"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kFileNotFound));
    }
}

void test_config_loading_and_missing_fields() {
    {
        auto r = numa_engine::load_pipeline_config_json(fixture_path("config_valid.json"));
        EXPECT_TRUE(r.ok());
        if (r.ok()) {
            EXPECT_EQ(r.value().scenario_name, "fixture_local_first_touch");
            EXPECT_EQ(r.value().queue_capacity, 1024U);
        }
    }
    {
        auto r = numa_engine::load_pipeline_config_json(fixture_path("config_invalid_missing_fields.json"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kInvalidConfig));
    }
    {
        auto r = numa_engine::load_pipeline_config_json(fixture_path("missing_config.json"));
        EXPECT_TRUE(!r.ok());
        EXPECT_EQ(static_cast<int>(r.error().code), static_cast<int>(numa_engine::ErrorCode::kFileNotFound));
    }
}

}  // namespace

int main() {
    test_valid_small_fixture_against_independent_expected_result();
    test_boundary_fixture();
    test_invalid_fixtures_rejected_with_structured_errors();
    test_config_loading_and_missing_fields();

    if (g_failures > 0) {
        std::cerr << "test_contracts_and_fixtures FAILED (" << g_failures << " assertion failures)\n";
        return 1;
    }
    std::cout << "test_contracts_and_fixtures PASSED\n";
    return 0;
}
