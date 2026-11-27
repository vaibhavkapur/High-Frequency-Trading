#include "numa_engine/affinity.hpp"

#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

namespace numa_engine {

int current_cpu_id() noexcept {
#if defined(__linux__)
    return sched_getcpu();
#else
    return -1;
#endif
}

Result<AffinityResult> pin_current_thread(
    int cpu_id,
    const TopologyInfo& topology,
    bool require_hard_pinning) {
    AffinityResult res;
    res.requested_cpu = cpu_id;

    if (cpu_id < 0) {
        res.requested_pinning = false;
        res.enforced = false;
        res.observed_cpu = current_cpu_id();
        res.mechanism = "unpinned";
        res.note = "Thread left unpinned (OS scheduler placement).";
        return res;
    }

    if (!topology.has_cpu(cpu_id)) {
        return Error{
            ErrorCode::kInvalidCpuId,
            "Requested CPU ID " + std::to_string(cpu_id) +
                " is out of range for topology (logical_cpus=" +
                std::to_string(topology.logical_cpus) + ")",
            "cpu_id",
            0};
    }

    res.requested_pinning = true;

#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(cpu_id), &cpuset);

    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        if (require_hard_pinning) {
            return Error{
                ErrorCode::kAffinityFailed,
                "pthread_setaffinity_np failed for CPU " + std::to_string(cpu_id) +
                    " (errno=" + std::to_string(rc) + ")",
                "cpu_id",
                0};
        }
        res.enforced = false;
        res.observed_cpu = current_cpu_id();
        res.mechanism = "linux_pthread_setaffinity_failed";
        res.note = "pthread_setaffinity_np failed (errno=" + std::to_string(rc) + "); continuing unpinned.";
        return res;
    }

    res.enforced = true;
    res.observed_cpu = current_cpu_id();
    res.mechanism = "linux_pthread_setaffinity_np";
    res.note = "Pinned to CPU " + std::to_string(cpu_id) +
               " (observed CPU " + std::to_string(res.observed_cpu) + ").";
    return res;

#elif defined(__APPLE__)
    if (require_hard_pinning) {
        return Error{
            ErrorCode::kUnavailablePlatformApi,
            "Hard CPU core pinning is unavailable on macOS (Mach THREAD_AFFINITY_POLICY is advisory only).",
            "cpu_id",
            0};
    }

    // Apply advisory Mach thread affinity tag; note explicitly that macOS does
    // not guarantee hard core binding like Linux sched_setaffinity.
    thread_affinity_policy_data_t policy{};
    policy.affinity_tag = static_cast<integer_t>(cpu_id + 1);
    const kern_return_t kr = thread_policy_set(
        mach_thread_self(),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);

    res.enforced = false;
    res.observed_cpu = -1;
    res.mechanism = "macos_mach_affinity_tag_advisory";
    res.note = (kr == KERN_SUCCESS)
        ? "macOS advisory affinity tag set (tag=" + std::to_string(cpu_id + 1) +
              "); hard core pinning is unavailable on macOS."
        : "macOS thread_policy_set returned status " + std::to_string(kr) +
              " (expected on Apple Silicon); hard core pinning unavailable.";
    return res;
#else
    if (require_hard_pinning) {
        return Error{
            ErrorCode::kUnavailablePlatformApi,
            "Hard CPU pinning is not implemented on this OS.",
            "cpu_id",
            0};
    }
    res.enforced = false;
    res.mechanism = "unsupported_os";
    res.note = "CPU pinning unavailable on this OS.";
    return res;
#endif
}

}  // namespace numa_engine
