#include "numa_engine/memory.hpp"

#include <cstring>
#include <sys/mman.h>
#include <utility>

#if defined(__linux__) && NUMA_ENGINE_HAS_LIBNUMA
#include <numa.h>
#include <numaif.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace numa_engine {

namespace {

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    if (alignment == 0) return value;
    const std::size_t rem = value % alignment;
    return rem == 0 ? value : (value + alignment - rem);
}

#if defined(__linux__) && !NUMA_ENGINE_HAS_LIBNUMA && defined(SYS_mbind)
// Linux MPOL constants (linux/mempolicy.h)
constexpr int kMpolDefault = 0;
constexpr int kMpolBind = 2;
constexpr int kMpolInterleave = 3;
constexpr unsigned kMpolMfStrict = 1U << 0;
constexpr unsigned kMpolMfMove = 1U << 1;

long invoke_mbind(void* addr, unsigned long len, int mode, const unsigned long* nodemask,
                  unsigned long maxnode, unsigned flags) {
    return syscall(SYS_mbind, addr, len, mode, nodemask, maxnode, flags);
}
#endif

}  // namespace

Result<AllocationReport> evaluate_memory_policy(
    const AllocationRequest& request,
    const TopologyInfo& topology) {
    if (request.bytes == 0) {
        return Error{ErrorCode::kInvalidConfig, "Allocation size must be > 0 bytes", "bytes", 0};
    }

    // For remote placement on a single-node host, report UNAVAILABLE_SINGLE_NODE
    // (or FALLBACK_SINGLE_NODE) before checking whether preferred_node exists.
    if (request.policy == MemoryPolicy::kRemote && !topology.is_multi_numa &&
        !request.allow_single_node_fallback) {
        return Error{
            ErrorCode::kUnavailableSingleNode,
            "Remote NUMA memory placement requires >= 2 NUMA nodes; host has " +
                std::to_string(topology.numa_node_count()) + " node.",
            "memory_policy",
            0};
    }

    if (request.policy != MemoryPolicy::kDefault && request.policy != MemoryPolicy::kInterleaved) {
        if (request.preferred_node < 0 || !topology.has_node(request.preferred_node)) {
            if (!request.allow_single_node_fallback || topology.is_multi_numa) {
                return Error{
                    ErrorCode::kInvalidNumaNode,
                    "Requested NUMA node " + std::to_string(request.preferred_node) +
                        " does not exist (available nodes: " +
                        std::to_string(topology.numa_node_count()) + ")",
                    "preferred_node",
                    0};
            }
        }
    }

    AllocationReport report;
    report.requested_policy = request.policy;
    report.effective_policy = request.policy;
    report.requested_node = request.preferred_node;
    report.effective_node = topology.has_node(request.preferred_node) ? request.preferred_node : 0;
    report.requested_bytes = request.bytes;
    report.mapped_bytes = align_up(request.bytes, std::max<std::size_t>(topology.page_size_bytes, request.alignment));
    report.first_touch_deferred = request.defer_first_touch;

    switch (request.policy) {
        case MemoryPolicy::kDefault:
            report.status = ScenarioStatus::kExecuted;
            report.numa_policy_enforced = false;
            report.note = "OS default virtual memory allocation (no explicit NUMA binding).";
            break;

        case MemoryPolicy::kFirstTouchLocal:
            report.status = topology.is_multi_numa ? ScenarioStatus::kExecuted : ScenarioStatus::kFallbackSingleNode;
            report.numa_policy_enforced = topology.is_multi_numa;
            report.note = topology.is_multi_numa
                ? "Anonymous mmap with deferred page fault on owning worker thread (first-touch local)."
                : "Single-node host: first-touch page fault runs on node 0 (explicitly labeled single-node fallback).";
            break;

        case MemoryPolicy::kLocal:
            if (!topology.supports_numa_memory_policy) {
                report.effective_policy = MemoryPolicy::kFirstTouchLocal;
                report.effective_node = 0;
                report.numa_policy_enforced = false;
                report.status = ScenarioStatus::kFallbackSingleNode;
                report.note = "Platform lacks Linux mbind/libnuma; fell back to anonymous mmap on single node 0.";
            } else if (!topology.is_multi_numa) {
                report.effective_node = 0;
                report.numa_policy_enforced = true;
                report.status = ScenarioStatus::kFallbackSingleNode;
                report.note = "Single-node Linux host: local policy binds to sole node 0.";
            } else {
                report.numa_policy_enforced = true;
                report.status = ScenarioStatus::kExecuted;
                report.note = "Bound to local NUMA node " + std::to_string(report.effective_node) + ".";
            }
            break;

        case MemoryPolicy::kRemote:
            if (!topology.is_multi_numa) {
                if (!request.allow_single_node_fallback) {
                    return Error{
                        ErrorCode::kUnavailableSingleNode,
                        "Remote NUMA memory placement requires >= 2 NUMA nodes; host has " +
                            std::to_string(topology.numa_node_count()) + " node.",
                        "memory_policy",
                        0};
                }
                report.effective_policy = MemoryPolicy::kDefault;
                report.effective_node = 0;
                report.numa_policy_enforced = false;
                report.status = ScenarioStatus::kFallbackSingleNode;
                report.note = "FALLBACK_SINGLE_NODE: host has 1 NUMA node; remote placement unavailable and NOT cross-node.";
            } else if (!topology.supports_numa_memory_policy) {
                if (!request.allow_single_node_fallback) {
                    return Error{
                        ErrorCode::kUnavailablePlatformApi,
                        "Remote NUMA placement requires Linux libnuma/mbind support.",
                        "memory_policy",
                        0};
                }
                report.effective_policy = MemoryPolicy::kDefault;
                report.numa_policy_enforced = false;
                report.status = ScenarioStatus::kUnavailablePlatform;
                report.note = "Platform lacks mbind/libnuma; remote NUMA binding not enforced.";
            } else {
                report.numa_policy_enforced = true;
                report.status = ScenarioStatus::kExecuted;
                report.note = "Bound to remote NUMA node " + std::to_string(report.effective_node) + ".";
            }
            break;

        case MemoryPolicy::kInterleaved:
            if (!topology.is_multi_numa) {
                if (!request.allow_single_node_fallback) {
                    return Error{
                        ErrorCode::kUnavailableSingleNode,
                        "Interleaved NUMA memory placement requires >= 2 NUMA nodes; host has " +
                            std::to_string(topology.numa_node_count()) + " node.",
                        "memory_policy",
                        0};
                }
                report.effective_policy = MemoryPolicy::kDefault;
                report.effective_node = 0;
                report.numa_policy_enforced = false;
                report.status = ScenarioStatus::kFallbackSingleNode;
                report.note = "FALLBACK_SINGLE_NODE: host has 1 NUMA node; interleaved placement unavailable and NOT multi-node.";
            } else if (!topology.supports_numa_memory_policy) {
                if (!request.allow_single_node_fallback) {
                    return Error{
                        ErrorCode::kUnavailablePlatformApi,
                        "Interleaved NUMA placement requires Linux libnuma/mbind support.",
                        "memory_policy",
                        0};
                }
                report.effective_policy = MemoryPolicy::kDefault;
                report.numa_policy_enforced = false;
                report.status = ScenarioStatus::kUnavailablePlatform;
                report.note = "Platform lacks mbind/libnuma; interleaved NUMA policy not enforced.";
            } else {
                report.numa_policy_enforced = true;
                report.status = ScenarioStatus::kExecuted;
                report.note = "Interleaved across " + std::to_string(topology.numa_node_count()) + " NUMA nodes.";
            }
            break;
    }

    return report;
}

NumaBuffer::~NumaBuffer() {
    release();
}

NumaBuffer::NumaBuffer(NumaBuffer&& other) noexcept
    : ptr_(other.ptr_),
      allocated_with_libnuma_(other.allocated_with_libnuma_),
      report_(std::move(other.report_)) {
    other.ptr_ = nullptr;
    other.allocated_with_libnuma_ = false;
}

NumaBuffer& NumaBuffer::operator=(NumaBuffer&& other) noexcept {
    if (this != &other) {
        release();
        ptr_ = other.ptr_;
        allocated_with_libnuma_ = other.allocated_with_libnuma_;
        report_ = std::move(other.report_);
        other.ptr_ = nullptr;
        other.allocated_with_libnuma_ = false;
    }
    return *this;
}

void NumaBuffer::release() noexcept {
    if (ptr_ == nullptr) return;
#if defined(__linux__) && NUMA_ENGINE_HAS_LIBNUMA
    if (allocated_with_libnuma_) {
        numa_free(ptr_, report_.mapped_bytes);
        ptr_ = nullptr;
        allocated_with_libnuma_ = false;
        return;
    }
#endif
    munmap(ptr_, report_.mapped_bytes);
    ptr_ = nullptr;
}

Result<NumaBuffer> NumaBuffer::allocate(
    const AllocationRequest& request,
    const TopologyInfo& topology) {
    auto eval = evaluate_memory_policy(request, topology);
    if (!eval.ok()) {
        return eval.error();
    }

    NumaBuffer buf;
    buf.report_ = std::move(eval.value());

#if defined(__linux__) && NUMA_ENGINE_HAS_LIBNUMA
    if (numa_available() >= 0 && buf.report_.numa_policy_enforced) {
        if (buf.report_.effective_policy == MemoryPolicy::kLocal ||
            buf.report_.effective_policy == MemoryPolicy::kRemote) {
            buf.ptr_ = numa_alloc_onnode(buf.report_.mapped_bytes, buf.report_.effective_node);
            if (buf.ptr_ != nullptr) {
                buf.allocated_with_libnuma_ = true;
                buf.report_.used_mmap = true;
            }
        } else if (buf.report_.effective_policy == MemoryPolicy::kInterleaved) {
            buf.ptr_ = numa_alloc_interleaved(buf.report_.mapped_bytes);
            if (buf.ptr_ != nullptr) {
                buf.allocated_with_libnuma_ = true;
                buf.report_.used_mmap = true;
            }
        }
    }
#endif

    if (buf.ptr_ == nullptr) {
        void* raw = mmap(
            nullptr,
            buf.report_.mapped_bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);
        if (raw == MAP_FAILED) {
            return Error{
                ErrorCode::kAllocationFailed,
                "mmap failed for " + std::to_string(buf.report_.mapped_bytes) + " bytes",
                "memory",
                0};
        }
        buf.ptr_ = raw;
        buf.report_.used_mmap = true;

#if defined(__linux__) && !NUMA_ENGINE_HAS_LIBNUMA && defined(SYS_mbind)
        if (buf.report_.numa_policy_enforced) {
            if (buf.report_.effective_policy == MemoryPolicy::kLocal ||
                buf.report_.effective_policy == MemoryPolicy::kRemote) {
                unsigned long nodemask = 1UL << static_cast<unsigned>(buf.report_.effective_node);
                const long rc = invoke_mbind(
                    buf.ptr_,
                    buf.report_.mapped_bytes,
                    kMpolBind,
                    &nodemask,
                    sizeof(nodemask) * 8,
                    kMpolMfStrict | kMpolMfMove);
                if (rc != 0) {
                    buf.report_.numa_policy_enforced = false;
                    buf.report_.note += " (mbind returned error; using OS default placement)";
                }
            } else if (buf.report_.effective_policy == MemoryPolicy::kInterleaved) {
                unsigned long nodemask = 0;
                for (const auto& n : topology.nodes) {
                    if (n.node_id >= 0 && n.node_id < 64) {
                        nodemask |= (1UL << static_cast<unsigned>(n.node_id));
                    }
                }
                const long rc = invoke_mbind(
                    buf.ptr_,
                    buf.report_.mapped_bytes,
                    kMpolInterleave,
                    &nodemask,
                    sizeof(nodemask) * 8,
                    kMpolMfStrict | kMpolMfMove);
                if (rc != 0) {
                    buf.report_.numa_policy_enforced = false;
                    buf.report_.note += " (mbind interleave returned error; using OS default)";
                }
            }
        }
#endif
    }

    if (!request.defer_first_touch) {
        buf.touch_pages(topology.page_size_bytes);
    }

    return buf;
}

void NumaBuffer::touch_pages(std::size_t page_size_bytes) noexcept {
    if (ptr_ == nullptr || report_.mapped_bytes == 0) return;
    const std::size_t stride = page_size_bytes > 0 ? page_size_bytes : 4096;
    auto* bytes = static_cast<volatile std::uint8_t*>(ptr_);
    for (std::size_t offset = 0; offset < report_.mapped_bytes; offset += stride) {
        bytes[offset] = 0;
    }
    // Zero entire region so buffer starts in a clean deterministic state.
    std::memset(ptr_, 0, report_.mapped_bytes);
    report_.first_touch_completed = true;
}

}  // namespace numa_engine
