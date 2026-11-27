#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "numa_engine/topology.hpp"
#include "numa_engine/types.hpp"

namespace numa_engine {

struct AllocationRequest {
    std::size_t bytes = 0;
    std::size_t alignment = kCacheLineSize;
    MemoryPolicy policy = MemoryPolicy::kDefault;
    int preferred_node = 0;
    bool defer_first_touch = false;  // If true, pages are left untouched until touch_pages() is called
    bool allow_single_node_fallback = false;
};

struct AllocationReport {
    MemoryPolicy requested_policy = MemoryPolicy::kDefault;
    MemoryPolicy effective_policy = MemoryPolicy::kDefault;
    int requested_node = 0;
    int effective_node = 0;
    std::size_t requested_bytes = 0;
    std::size_t mapped_bytes = 0;
    bool used_mmap = false;
    bool first_touch_deferred = false;
    bool first_touch_completed = false;
    bool numa_policy_enforced = false;
    ScenarioStatus status = ScenarioStatus::kExecuted;
    std::string note;
};

// Validates whether an AllocationRequest is supported on the given topology.
[[nodiscard]] Result<AllocationReport> evaluate_memory_policy(
    const AllocationRequest& request,
    const TopologyInfo& topology);

// RAII page-aligned memory region supporting first-touch initialization and
// Linux NUMA memory binding (libnuma or mbind) with explicit fallback reporting.
class NumaBuffer {
public:
    NumaBuffer() = default;
    ~NumaBuffer();

    NumaBuffer(const NumaBuffer&) = delete;
    NumaBuffer& operator=(const NumaBuffer&) = delete;
    NumaBuffer(NumaBuffer&& other) noexcept;
    NumaBuffer& operator=(NumaBuffer&& other) noexcept;

    [[nodiscard]] static Result<NumaBuffer> allocate(
        const AllocationRequest& request,
        const TopologyInfo& topology);

    // Touches every physical page in the mapping from the calling thread.
    // Must be called AFTER pinning the owning thread to enforce first-touch
    // page placement on the caller's local NUMA node.
    void touch_pages(std::size_t page_size_bytes) noexcept;

    [[nodiscard]] void* data() noexcept { return ptr_; }
    [[nodiscard]] const void* data() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return report_.mapped_bytes; }
    [[nodiscard]] const AllocationReport& report() const noexcept { return report_; }
    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }

private:
    void release() noexcept;

    void* ptr_ = nullptr;
    bool allocated_with_libnuma_ = false;
    AllocationReport report_{};
};

}  // namespace numa_engine
