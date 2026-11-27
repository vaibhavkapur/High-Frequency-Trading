#include <iostream>
#include <vector>

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

void test_functional_equivalence_across_all_placements() {
    using namespace numa_engine;

    GeneratorConfig gcfg;
    gcfg.seed = 1337;
    gcfg.num_events = 25000;
    gcfg.warmup_events = 1000;
    gcfg.num_symbols = 16;

    auto ev_res = generate_synthetic_events(gcfg);
    EXPECT_TRUE(ev_res.ok());
    if (!ev_res.ok()) return;
    const auto& events = ev_res.value();

    // Deterministic single-threaded domain reference
    MatchingEngine ref_engine;
    for (const auto& ev : events) {
        (void)ref_engine.process(ev);
    }
    const EngineSummary ref_summary = ref_engine.summary();

    const TopologyInfo topo = detect_topology();
    const int cpu0 = 0;
    const int cpu1 = topo.logical_cpus > 1 ? 1 : 0;

    std::vector<PipelineConfig> configs;
    {
        PipelineConfig c;
        c.scenario_name = "baseline_mutex";
        c.pipeline_kind = PipelineKind::kBaselineMutex;
        c.memory_policy = MemoryPolicy::kDefault;
        c.queue_capacity = 1024;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_default";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kDefault;
        c.queue_capacity = 1024;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_local_pinned";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kLocal;
        c.queue_capacity = 1024;
        c.producer_cpu = cpu0;
        c.consumer_cpu = cpu1;
        c.preferred_memory_node = 0;
        c.allow_single_node_fallback = true;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_first_touch_consumer";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kFirstTouchLocal;
        c.queue_capacity = 1024;
        c.producer_cpu = cpu0;
        c.consumer_cpu = cpu1;
        c.consumer_first_touch = true;
        c.allow_single_node_fallback = true;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_first_touch_producer";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kFirstTouchLocal;
        c.queue_capacity = 1024;
        c.producer_cpu = cpu0;
        c.consumer_cpu = cpu1;
        c.consumer_first_touch = false;
        c.allow_single_node_fallback = true;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_remote_fallback";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kRemote;
        c.queue_capacity = 1024;
        c.producer_cpu = cpu0;
        c.consumer_cpu = cpu1;
        c.preferred_memory_node = topo.is_multi_numa ? 1 : 0;
        c.allow_single_node_fallback = true;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }
    {
        PipelineConfig c;
        c.scenario_name = "spsc_interleaved_fallback";
        c.pipeline_kind = PipelineKind::kLockFreeSpsc;
        c.memory_policy = MemoryPolicy::kInterleaved;
        c.queue_capacity = 1024;
        c.producer_cpu = cpu0;
        c.consumer_cpu = cpu1;
        c.allow_single_node_fallback = true;
        c.seed = gcfg.seed;
        configs.push_back(c);
    }

    for (const auto& cfg : configs) {
        auto run = run_pipeline(cfg, events, topo);
        EXPECT_TRUE(run.ok());
        if (!run.ok()) continue;

        EXPECT_EQ(run.value().lost_events, 0ULL);
        EXPECT_EQ(run.value().events_sent, events.size());
        EXPECT_EQ(run.value().events_received, events.size());
        EXPECT_EQ(run.value().measured_events, gcfg.num_events);
        EXPECT_EQ(run.value().warmup_events, gcfg.warmup_events);
        EXPECT_TRUE(run.value().engine_summary == ref_summary);
    }
}

void test_reproducibility_with_fixed_seed() {
    using namespace numa_engine;

    GeneratorConfig gcfg;
    gcfg.seed = 20261127ULL;
    gcfg.num_events = 10000;
    gcfg.warmup_events = 500;
    gcfg.num_symbols = 8;

    auto e1 = generate_synthetic_events(gcfg);
    auto e2 = generate_synthetic_events(gcfg);
    EXPECT_TRUE(e1.ok() && e2.ok());
    if (!e1.ok() || !e2.ok()) return;

    EXPECT_EQ(e1.value().size(), e2.value().size());
    for (std::size_t i = 0; i < e1.value().size(); ++i) {
        EXPECT_EQ(e1.value()[i].seq_id, e2.value()[i].seq_id);
        EXPECT_EQ(e1.value()[i].symbol_id, e2.value()[i].symbol_id);
        EXPECT_EQ(e1.value()[i].price_ticks, e2.value()[i].price_ticks);
        EXPECT_EQ(e1.value()[i].quantity, e2.value()[i].quantity);
        EXPECT_EQ(e1.value()[i].event_type, e2.value()[i].event_type);
        EXPECT_EQ(e1.value()[i].side, e2.value()[i].side);
    }

    const TopologyInfo topo = detect_topology();
    ComparisonRunConfig ccfg;
    ccfg.num_events = 10000;
    ccfg.warmup_events = 500;
    ccfg.queue_capacity = 1024;
    ccfg.seed = 20261127ULL;
    ccfg.repetitions = 2;

    auto r1 = run_comparison_suite(ccfg, topo);
    auto r2 = run_comparison_suite(ccfg, topo);
    EXPECT_TRUE(r1.ok() && r2.ok());
    if (r1.ok() && r2.ok()) {
        EXPECT_EQ(r1.value().report_id, r2.value().report_id);
        EXPECT_EQ(r1.value().baseline_digest, r2.value().baseline_digest);
        EXPECT_TRUE(r1.value().functional_equivalence_passed);
        EXPECT_TRUE(r1.value().zero_lost_events);
    }
}

}  // namespace

int main() {
    test_functional_equivalence_across_all_placements();
    test_reproducibility_with_fixed_seed();

    if (g_failures > 0) {
        std::cerr << "test_domain_equivalence FAILED (" << g_failures << " assertion failures)\n";
        return 1;
    }
    std::cout << "test_domain_equivalence PASSED\n";
    return 0;
}
