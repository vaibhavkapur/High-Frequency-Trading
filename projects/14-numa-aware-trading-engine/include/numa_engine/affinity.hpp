#pragma once

#include <string>

#include "numa_engine/topology.hpp"
#include "numa_engine/types.hpp"

namespace numa_engine {

struct AffinityResult {
    int requested_cpu = -1;
    int observed_cpu = -1;
    bool requested_pinning = false;
    bool enforced = false;
    std::string mechanism;
    std::string note;
};

// Pins the calling thread to cpu_id when >= 0.
// - On Linux: uses pthread_setaffinity_np / sched_setaffinity and verifies with sched_getcpu().
// - On macOS: Mach THREAD_AFFINITY_POLICY is advisory only; returns enforced = false with
//   explicit diagnostics rather than falsely claiming hard core pinning.
// - If cpu_id < 0, leaves thread unpinned and succeeds cleanly.
[[nodiscard]] Result<AffinityResult> pin_current_thread(
    int cpu_id,
    const TopologyInfo& topology,
    bool require_hard_pinning = false);

// Returns current logical CPU ID when supported by the OS, or -1 if unavailable.
[[nodiscard]] int current_cpu_id() noexcept;

}  // namespace numa_engine
