#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace numa_engine {

inline constexpr std::string_view kSchemaVersion = "1.0.0";
inline constexpr std::size_t kCacheLineSize = 128;  // Safe across 64B (x86_64) and 128B (Apple Silicon / prefetcher)
inline constexpr std::uint16_t kMaxSymbols = 64;
inline constexpr std::size_t kMinQueueCapacity = 8;
inline constexpr std::size_t kMaxQueueCapacity = 1U << 22;  // 4M slots

enum class EventType : std::uint8_t {
    kAdd = 1,
    kModify = 2,
    kCancel = 3,
    kTrade = 4
};

enum class Side : std::uint8_t {
    kBid = 1,
    kAsk = 2
};

enum class MemoryPolicy : std::uint8_t {
    kDefault = 0,          // OS default allocation without NUMA binding
    kFirstTouchLocal = 1,  // Anonymous mmap touched by target worker thread after pinning
    kLocal = 2,            // Explicitly bound to target NUMA node (libnuma / mbind)
    kRemote = 3,           // Explicitly bound to a different NUMA node than consumer
    kInterleaved = 4       // Interleaved across all online NUMA nodes
};

enum class PipelineKind : std::uint8_t {
    kBaselineMutex = 0,    // Reference mutex + condition_variable + deque baseline
    kLockFreeSpsc = 1      // Bounded lock-free SPSC ring buffer with NUMA placement
};

enum class ScenarioStatus : std::uint8_t {
    kExecuted = 0,               // Ran natively with full requested capabilities
    kFallbackSingleNode = 1,     // Host has 1 NUMA node; executed with explicit fallback label
    kUnavailableSingleNode = 2,  // Strictly skipped/rejected because multi-NUMA is unavailable
    kUnavailablePlatform = 3,    // Platform lacks required kernel API (e.g., macOS hard pinning/mbind)
    kFailedValidation = 4        // Configuration or input failed contract validation
};

enum class ErrorCode : std::uint16_t {
    kOk = 0,
    kInvalidConfig = 100,
    kInvalidQueueCapacity = 101,
    kInvalidCpuId = 102,
    kInvalidNumaNode = 103,
    kInvalidEventSchema = 200,
    kNonMonotonicSequence = 201,
    kInvalidPrice = 202,
    kInvalidQuantity = 203,
    kInvalidSymbol = 204,
    kInvalidEventType = 205,
    kInvalidSide = 206,
    kFileNotFound = 300,
    kParseError = 301,
    kUnavailableSingleNode = 400,
    kUnavailablePlatformApi = 401,
    kAllocationFailed = 500,
    kAffinityFailed = 501,
    kEventLossDetected = 600
};

struct Error {
    ErrorCode code = ErrorCode::kOk;
    std::string message;
    std::string field;
    std::size_t line = 0;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }
};

template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return value_.has_value() && error_.ok(); }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    Error error_{};
};

// Fixed-size 64-byte cache-line-aligned synthetic market event.
// Price is expressed in integer ticks (1 tick = 0.0001 USD) to guarantee
// cross-platform bit-exact determinism.
struct alignas(64) MarketEvent {
    std::uint64_t seq_id = 0;       // Strictly monotonically increasing starting at 1
    std::uint64_t send_ts_ns = 0;   // Monotonic producer timestamp in nanoseconds
    std::int64_t price_ticks = 0;   // Must be > 0 for Add/Modify/Trade; >= 0 for Cancel
    std::uint32_t quantity = 0;     // Must be > 0 for Add/Modify/Trade; >= 0 for Cancel
    std::uint16_t symbol_id = 0;    // [0, kMaxSymbols)
    std::uint8_t event_type = 0;    // EventType
    std::uint8_t side = 0;          // Side
    std::uint64_t order_id = 0;     // Deterministic synthetic order identifier
    std::uint32_t flags = 0;        // Bit 0: warmup event (excluded from metrics)
    std::uint32_t reserved = 0;
    std::uint64_t pad[2] = {0, 0};
};

static_assert(sizeof(MarketEvent) == 64, "MarketEvent must be exactly 64 bytes");
static_assert(alignof(MarketEvent) == 64, "MarketEvent must be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<MarketEvent>, "MarketEvent must be trivially copyable");
static_assert(std::is_standard_layout_v<MarketEvent>, "MarketEvent must be standard layout");

inline constexpr std::uint32_t kEventFlagWarmup = 1U << 0;

struct alignas(64) ProcessedEvent {
    std::uint64_t seq_id = 0;
    std::uint64_t send_ts_ns = 0;
    std::uint64_t recv_ts_ns = 0;
    std::uint64_t done_ts_ns = 0;
    std::int64_t best_bid_ticks = 0;
    std::int64_t best_ask_ticks = 0;
    std::uint32_t accepted_qty = 0;
    std::uint16_t symbol_id = 0;
    std::uint8_t status = 0;  // 0 = applied, 1 = no-op (e.g., cancel on empty side)
    std::uint8_t side = 0;
    std::uint64_t state_digest = 0;
};

static_assert(sizeof(ProcessedEvent) == 64, "ProcessedEvent must be exactly 64 bytes");
static_assert(std::is_trivially_copyable_v<ProcessedEvent>, "ProcessedEvent must be trivially copyable");

[[nodiscard]] inline std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::kAdd: return "ADD";
        case EventType::kModify: return "MODIFY";
        case EventType::kCancel: return "CANCEL";
        case EventType::kTrade: return "TRADE";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline std::string_view to_string(Side s) noexcept {
    switch (s) {
        case Side::kBid: return "BID";
        case Side::kAsk: return "ASK";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline std::string_view to_string(MemoryPolicy p) noexcept {
    switch (p) {
        case MemoryPolicy::kDefault: return "default";
        case MemoryPolicy::kFirstTouchLocal: return "first_touch_local";
        case MemoryPolicy::kLocal: return "local";
        case MemoryPolicy::kRemote: return "remote";
        case MemoryPolicy::kInterleaved: return "interleaved";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view to_string(PipelineKind k) noexcept {
    switch (k) {
        case PipelineKind::kBaselineMutex: return "baseline_mutex";
        case PipelineKind::kLockFreeSpsc: return "lock_free_spsc";
    }
    return "unknown";
}

[[nodiscard]] inline std::string_view to_string(ScenarioStatus s) noexcept {
    switch (s) {
        case ScenarioStatus::kExecuted: return "EXECUTED";
        case ScenarioStatus::kFallbackSingleNode: return "FALLBACK_SINGLE_NODE";
        case ScenarioStatus::kUnavailableSingleNode: return "UNAVAILABLE_SINGLE_NODE";
        case ScenarioStatus::kUnavailablePlatform: return "UNAVAILABLE_PLATFORM";
        case ScenarioStatus::kFailedValidation: return "FAILED_VALIDATION";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::kOk: return "OK";
        case ErrorCode::kInvalidConfig: return "INVALID_CONFIG";
        case ErrorCode::kInvalidQueueCapacity: return "INVALID_QUEUE_CAPACITY";
        case ErrorCode::kInvalidCpuId: return "INVALID_CPU_ID";
        case ErrorCode::kInvalidNumaNode: return "INVALID_NUMA_NODE";
        case ErrorCode::kInvalidEventSchema: return "INVALID_EVENT_SCHEMA";
        case ErrorCode::kNonMonotonicSequence: return "NON_MONOTONIC_SEQUENCE";
        case ErrorCode::kInvalidPrice: return "INVALID_PRICE";
        case ErrorCode::kInvalidQuantity: return "INVALID_QUANTITY";
        case ErrorCode::kInvalidSymbol: return "INVALID_SYMBOL";
        case ErrorCode::kInvalidEventType: return "INVALID_EVENT_TYPE";
        case ErrorCode::kInvalidSide: return "INVALID_SIDE";
        case ErrorCode::kFileNotFound: return "FILE_NOT_FOUND";
        case ErrorCode::kParseError: return "PARSE_ERROR";
        case ErrorCode::kUnavailableSingleNode: return "UNAVAILABLE_SINGLE_NODE";
        case ErrorCode::kUnavailablePlatformApi: return "UNAVAILABLE_PLATFORM_API";
        case ErrorCode::kAllocationFailed: return "ALLOCATION_FAILED";
        case ErrorCode::kAffinityFailed: return "AFFINITY_FAILED";
        case ErrorCode::kEventLossDetected: return "EVENT_LOSS_DETECTED";
    }
    return "UNKNOWN_ERROR";
}

[[nodiscard]] inline std::optional<MemoryPolicy> parse_memory_policy(std::string_view s) noexcept {
    if (s == "default") return MemoryPolicy::kDefault;
    if (s == "first_touch_local") return MemoryPolicy::kFirstTouchLocal;
    if (s == "local") return MemoryPolicy::kLocal;
    if (s == "remote") return MemoryPolicy::kRemote;
    if (s == "interleaved") return MemoryPolicy::kInterleaved;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<PipelineKind> parse_pipeline_kind(std::string_view s) noexcept {
    if (s == "baseline_mutex" || s == "baseline") return PipelineKind::kBaselineMutex;
    if (s == "lock_free_spsc" || s == "spsc") return PipelineKind::kLockFreeSpsc;
    return std::nullopt;
}

[[nodiscard]] inline constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n >= 2 && (n & (n - 1)) == 0;
}

}  // namespace numa_engine
