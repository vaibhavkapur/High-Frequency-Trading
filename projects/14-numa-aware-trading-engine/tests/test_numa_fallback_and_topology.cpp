#include <iostream>

#include "numa_engine/affinity.hpp"
#include "numa_engine/event_generator.hpp"
#include "numa_engine/memory.hpp"
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

void test_linux_cpulist_parser() {
    const auto cpus = numa_engine::parse_linux_cpulist("0-3,8,10-12");
    EXPECT_EQ(cpus.size(), 8U);
    if (cpus.size() == 8U) {
        EXPECT_EQ(cpus[0], 0);
        EXPECT_EQ(cpus[3], 3);
        EXPECT_EQ(cpus[4], 8);
        EXPECT_EQ(cpus[5], 10);
        EXPECT_EQ(cpus[7], 12);
    }
}

void test_single_node_strict_rejection_and_explicit_fallback_labeling() {
    using namespace numa_engine;

    const TopologyInfo single_node = make_synthetic_topology(1, 8, false, false);
    EXPECT_TRUE(!single_node.is_multi_numa);
    EXPECT_EQ(single_node.numa_node_count(), 1U);

    // 1. Strict mode rejects Remote on single-node machine (no silent success!)
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kRemote;
        req.preferred_node = 1;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, single_node);
        EXPECT_TRUE(!res.ok());
        // Node 1 does not exist on 1-node machine -> explicit error
        EXPECT_TRUE(res.error().code == ErrorCode::kInvalidNumaNode ||
                    res.error().code == ErrorCode::kUnavailableSingleNode);
    }
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kRemote;
        req.preferred_node = 0;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, single_node);
        EXPECT_TRUE(!res.ok());
        EXPECT_EQ(static_cast<int>(res.error().code), static_cast<int>(ErrorCode::kUnavailableSingleNode));
    }
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kInterleaved;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, single_node);
        EXPECT_TRUE(!res.ok());
        EXPECT_EQ(static_cast<int>(res.error().code), static_cast<int>(ErrorCode::kUnavailableSingleNode));
    }

    // 2. Fallback mode labels scenario explicitly as FALLBACK_SINGLE_NODE
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kRemote;
        req.preferred_node = 1;
        req.allow_single_node_fallback = true;
        auto res = evaluate_memory_policy(req, single_node);
        EXPECT_TRUE(res.ok());
        if (res.ok()) {
            EXPECT_EQ(static_cast<int>(res.value().status), static_cast<int>(ScenarioStatus::kFallbackSingleNode));
            EXPECT_TRUE(!res.value().numa_policy_enforced);
            EXPECT_EQ(res.value().effective_node, 0);
        }
    }
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kInterleaved;
        req.allow_single_node_fallback = true;
        auto res = evaluate_memory_policy(req, single_node);
        EXPECT_TRUE(res.ok());
        if (res.ok()) {
            EXPECT_EQ(static_cast<int>(res.value().status), static_cast<int>(ScenarioStatus::kFallbackSingleNode));
            EXPECT_TRUE(!res.value().numa_policy_enforced);
        }
    }

    // 3. Comparison suite on single-node topology marks remote & interleaved UNAVAILABLE_SINGLE_NODE
    ComparisonRunConfig ccfg;
    ccfg.num_events = 2000;
    ccfg.warmup_events = 100;
    ccfg.queue_capacity = 256;
    ccfg.repetitions = 2;
    ccfg.allow_single_node_fallback = false;

    auto cmp = run_comparison_suite(ccfg, single_node);
    EXPECT_TRUE(cmp.ok());
    if (cmp.ok()) {
        EXPECT_TRUE(cmp.value().functional_equivalence_passed);
        EXPECT_TRUE(cmp.value().zero_lost_events);

        bool saw_remote_unavail = false;
        bool saw_interleaved_unavail = false;
        for (const auto& s : cmp.value().scenarios) {
            if (s.scenario_name == "spsc_remote_node") {
                saw_remote_unavail = (s.status == ScenarioStatus::kUnavailableSingleNode ||
                                      s.status == ScenarioStatus::kFailedValidation) &&
                                     (s.repetitions == 0);
            }
            if (s.scenario_name == "spsc_interleaved") {
                saw_interleaved_unavail = (s.status == ScenarioStatus::kUnavailableSingleNode) &&
                                          (s.repetitions == 0);
            }
        }
        EXPECT_TRUE(saw_remote_unavail);
        EXPECT_TRUE(saw_interleaved_unavail);
    }
}

void test_synthetic_two_node_topology_policy_validation() {
    using namespace numa_engine;

    const TopologyInfo dual_node = make_synthetic_topology(2, 8, true, true);
    EXPECT_TRUE(dual_node.is_multi_numa);
    EXPECT_EQ(dual_node.numa_node_count(), 2U);
    EXPECT_EQ(dual_node.node_for_cpu(0), 0);
    EXPECT_EQ(dual_node.node_for_cpu(8), 1);

    // Remote and Interleaved policies validate cleanly on a 2-node Linux topology
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kRemote;
        req.preferred_node = 1;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, dual_node);
        EXPECT_TRUE(res.ok());
        if (res.ok()) {
            EXPECT_EQ(static_cast<int>(res.value().status), static_cast<int>(ScenarioStatus::kExecuted));
            EXPECT_TRUE(res.value().numa_policy_enforced);
            EXPECT_EQ(res.value().effective_node, 1);
        }
    }
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kInterleaved;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, dual_node);
        EXPECT_TRUE(res.ok());
        if (res.ok()) {
            EXPECT_EQ(static_cast<int>(res.value().status), static_cast<int>(ScenarioStatus::kExecuted));
            EXPECT_TRUE(res.value().numa_policy_enforced);
        }
    }
    // Out-of-range node still fails explicitly
    {
        AllocationRequest req;
        req.bytes = 65536;
        req.policy = MemoryPolicy::kRemote;
        req.preferred_node = 5;
        req.allow_single_node_fallback = false;
        auto res = evaluate_memory_policy(req, dual_node);
        EXPECT_TRUE(!res.ok());
        EXPECT_EQ(static_cast<int>(res.error().code), static_cast<int>(ErrorCode::kInvalidNumaNode));
    }
}

void test_invalid_cpu_and_strict_pinning_error_handling() {
    using namespace numa_engine;

    const TopologyInfo host = detect_topology();
    auto bad_cpu = pin_current_thread(99999, host, false);
    EXPECT_TRUE(!bad_cpu.ok());
    EXPECT_EQ(static_cast<int>(bad_cpu.error().code), static_cast<int>(ErrorCode::kInvalidCpuId));

#if defined(__APPLE__)
    // On macOS, require_hard_pinning must fail explicitly rather than silently succeeding
    auto strict = pin_current_thread(0, host, true);
    EXPECT_TRUE(!strict.ok());
    EXPECT_EQ(static_cast<int>(strict.error().code), static_cast<int>(ErrorCode::kUnavailablePlatformApi));
#endif
}

}  // namespace

int main() {
    test_linux_cpulist_parser();
    test_single_node_strict_rejection_and_explicit_fallback_labeling();
    test_synthetic_two_node_topology_policy_validation();
    test_invalid_cpu_and_strict_pinning_error_handling();

    if (g_failures > 0) {
        std::cerr << "test_numa_fallback_and_topology FAILED (" << g_failures << " assertion failures)\n";
        return 1;
    }
    std::cout << "test_numa_fallback_and_topology PASSED\n";
    return 0;
}
