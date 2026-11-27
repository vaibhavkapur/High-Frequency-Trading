#include <benchmark/benchmark.h>

#include "numa_engine/event_generator.hpp"
#include "numa_engine/pipeline.hpp"
#include "numa_engine/topology.hpp"

namespace {

using namespace numa_engine;

void BM_PipelinePlacement(benchmark::State& state, PipelineKind kind, MemoryPolicy policy) {
    const TopologyInfo topo = detect_topology();

    GeneratorConfig gcfg;
    gcfg.seed = 42;
    gcfg.num_events = 50000;
    gcfg.warmup_events = 2000;
    gcfg.num_symbols = 16;
    auto ev_res = generate_synthetic_events(gcfg);
    if (!ev_res.ok()) {
        state.SkipWithError(ev_res.error().message.c_str());
        return;
    }

    PipelineConfig pcfg;
    pcfg.scenario_name = std::string(to_string(kind)) + "_" + std::string(to_string(policy));
    pcfg.pipeline_kind = kind;
    pcfg.memory_policy = policy;
    pcfg.queue_capacity = 4096;
    pcfg.producer_cpu = (policy == MemoryPolicy::kDefault) ? -1 : 0;
    pcfg.consumer_cpu = (policy == MemoryPolicy::kDefault) ? -1 : (topo.logical_cpus > 1 ? 1 : 0);
    pcfg.preferred_memory_node = (policy == MemoryPolicy::kRemote && topo.is_multi_numa) ? 1 : 0;
    pcfg.allow_single_node_fallback = false;

    const Error err = validate_pipeline_config(pcfg, topo);
    if (!err.ok()) {
        state.SkipWithMessage(err.message.c_str());
        return;
    }

    double last_p99 = 0.0;
    double last_p99_9 = 0.0;
    for (auto _ : state) {
        auto run = run_pipeline(pcfg, ev_res.value(), topo);
        if (!run.ok()) {
            state.SkipWithError(run.error().message.c_str());
            return;
        }
        last_p99 = static_cast<double>(run.value().e2e_latency.p99_ns);
        last_p99_9 = static_cast<double>(run.value().e2e_latency.p99_9_ns);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(gcfg.num_events));
    state.counters["p99_ns"] = last_p99;
    state.counters["p99_9_ns"] = last_p99_9;
}

BENCHMARK_CAPTURE(BM_PipelinePlacement, BaselineMutex, PipelineKind::kBaselineMutex, MemoryPolicy::kDefault);
BENCHMARK_CAPTURE(BM_PipelinePlacement, SpscDefault, PipelineKind::kLockFreeSpsc, MemoryPolicy::kDefault);
BENCHMARK_CAPTURE(BM_PipelinePlacement, SpscLocal, PipelineKind::kLockFreeSpsc, MemoryPolicy::kLocal);
BENCHMARK_CAPTURE(BM_PipelinePlacement, SpscFirstTouch, PipelineKind::kLockFreeSpsc, MemoryPolicy::kFirstTouchLocal);
BENCHMARK_CAPTURE(BM_PipelinePlacement, SpscRemote, PipelineKind::kLockFreeSpsc, MemoryPolicy::kRemote);
BENCHMARK_CAPTURE(BM_PipelinePlacement, SpscInterleaved, PipelineKind::kLockFreeSpsc, MemoryPolicy::kInterleaved);

}  // namespace

BENCHMARK_MAIN();
