#include <iostream>

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

void test_producer_consumer_backpressure_spsc_and_baseline() {
    using namespace numa_engine;

    GeneratorConfig gcfg;
    gcfg.seed = 9001;
    gcfg.num_events = 20000;
    gcfg.warmup_events = 500;
    gcfg.num_symbols = 8;

    auto ev_res = generate_synthetic_events(gcfg);
    EXPECT_TRUE(ev_res.ok());
    if (!ev_res.ok()) return;
    const auto& events = ev_res.value();

    MatchingEngine ref;
    for (const auto& ev : events) (void)ref.process(ev);
    const auto ref_summary = ref.summary();

    const TopologyInfo topo = detect_topology();

    // Deliberately tiny bounded ring (capacity=16) + synthetic consumer work to
    // guarantee sustained producer backpressure while asserting zero lost events
    // and strict FIFO functional equivalence.
    PipelineConfig spsc_cfg;
    spsc_cfg.scenario_name = "backpressure_spsc_cap16";
    spsc_cfg.pipeline_kind = PipelineKind::kLockFreeSpsc;
    spsc_cfg.memory_policy = MemoryPolicy::kFirstTouchLocal;
    spsc_cfg.queue_capacity = 16;
    spsc_cfg.producer_cpu = 0;
    spsc_cfg.consumer_cpu = topo.logical_cpus > 1 ? 1 : 0;
    spsc_cfg.consumer_work_iters = 60;
    spsc_cfg.allow_single_node_fallback = true;
    spsc_cfg.seed = gcfg.seed;

    auto spsc_res = run_pipeline(spsc_cfg, events, topo);
    EXPECT_TRUE(spsc_res.ok());
    if (spsc_res.ok()) {
        const auto& r = spsc_res.value();
        EXPECT_EQ(r.lost_events, 0ULL);
        EXPECT_EQ(r.events_sent, events.size());
        EXPECT_EQ(r.events_received, events.size());
        EXPECT_TRUE(r.engine_summary == ref_summary);
        EXPECT_TRUE(r.engine_summary.sequence_strictly_monotonic);
        EXPECT_TRUE(r.backpressure.producer_full_spins > 0ULL);
        EXPECT_TRUE(r.backpressure.max_observed_occupancy <= spsc_cfg.queue_capacity);
        EXPECT_EQ(r.backpressure.enqueued_total, events.size());
        EXPECT_EQ(r.backpressure.dequeued_total, events.size());
    }

    // Same backpressure scenario on baseline mutex pipeline
    PipelineConfig base_cfg = spsc_cfg;
    base_cfg.scenario_name = "backpressure_baseline_cap16";
    base_cfg.pipeline_kind = PipelineKind::kBaselineMutex;
    base_cfg.memory_policy = MemoryPolicy::kDefault;
    base_cfg.producer_cpu = -1;
    base_cfg.consumer_cpu = -1;

    auto base_res = run_pipeline(base_cfg, events, topo);
    EXPECT_TRUE(base_res.ok());
    if (base_res.ok()) {
        const auto& r = base_res.value();
        EXPECT_EQ(r.lost_events, 0ULL);
        EXPECT_TRUE(r.engine_summary == ref_summary);
        EXPECT_TRUE(r.backpressure.max_observed_occupancy <= base_cfg.queue_capacity);
        EXPECT_TRUE(r.backpressure.producer_full_spins > 0ULL);
    }
}

void test_invalid_queue_capacity_rejected() {
    using namespace numa_engine;

    const TopologyInfo topo = detect_topology();
    PipelineConfig cfg;
    cfg.scenario_name = "bad_capacity";
    cfg.pipeline_kind = PipelineKind::kLockFreeSpsc;
    cfg.queue_capacity = 100;  // Not a power of two!

    const Error err = validate_pipeline_config(cfg, topo);
    EXPECT_TRUE(!err.ok());
    EXPECT_EQ(static_cast<int>(err.code), static_cast<int>(ErrorCode::kInvalidQueueCapacity));
}

}  // namespace

int main() {
    test_producer_consumer_backpressure_spsc_and_baseline();
    test_invalid_queue_capacity_rejected();

    if (g_failures > 0) {
        std::cerr << "test_backpressure FAILED (" << g_failures << " assertion failures)\n";
        return 1;
    }
    std::cout << "test_backpressure PASSED\n";
    return 0;
}
