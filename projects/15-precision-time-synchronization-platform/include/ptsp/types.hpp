#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ptsp {

inline constexpr std::string_view kSchemaVersion = "ptsp.v1";

// Canonical nanosecond timestamp type (signed 64-bit integer, nanoseconds since domain epoch).
using Nanos = std::int64_t;

enum class ErrorCode : std::uint32_t {
    Ok = 0,
    InvalidConfig = 1,
    MissingConfig = 2,
    InvalidTimestamp = 3,
    ClockDomainMismatch = 4,
    UnsupportedClockDomain = 5,
    InvalidSyncSample = 6,
    NonMonotonicCounter = 7,
    HardwareTimestampUnavailable = 8,
    UnsupportedPlatform = 9,
    SkewViolation = 10,
    CausalViolation = 11,
    AmbiguousOrdering = 12,
    BufferCapacityExceeded = 13,
    FailClosedHalted = 14,
    IoError = 15,
    ParseError = 16,
    SchemaVersionMismatch = 17,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

enum class ClockDomain : std::uint8_t {
    Unknown = 0,
    Tai = 1,            // International Atomic Time (PTP grandmaster canonical domain)
    Utc = 2,            // Coordinated Universal Time (leap-second aware, distinct from TAI)
    MonotonicRaw = 3,   // Local unadjusted monotonic hardware oscillator
    Monotonic = 4,      // Local NTP/PTP slewed monotonic clock
    ExchangeVenue = 5,  // Venue-assigned matching engine clock domain
};

[[nodiscard]] std::string_view to_string(ClockDomain domain) noexcept;
[[nodiscard]] std::optional<ClockDomain> parse_clock_domain(std::string_view s) noexcept;

enum class TimestampRole : std::uint8_t {
    EventTime = 0,    // Time at which the market event occurred at its authoritative source
    ReceiveTime = 1,  // Time at which the local host/NIC observed the packet
    TransmitTime = 2, // Time at which the sender transmitted a sync or data packet
};

[[nodiscard]] std::string_view to_string(TimestampRole role) noexcept;
[[nodiscard]] std::optional<TimestampRole> parse_timestamp_role(std::string_view s) noexcept;

enum class TimestampCaptureMethod : std::uint8_t {
    Unknown = 0,
    HardwareNicPtp = 1,     // NIC PHY/MAC hardware timestamp (e.g., SO_TIMESTAMPING RAW_HARDWARE)
    KernelSoftware = 2,     // OS kernel socket/packet timestamp (SO_TIMESTAMPNS / SO_TIMESTAMPING SOFTWARE)
    UserspaceClock = 3,     // Userspace clock_gettime() call
    SyntheticFixture = 4,   // Offline deterministic synthetic fixture
    RecordedReplay = 5,     // Replayed from a versioned capture file
};

[[nodiscard]] std::string_view to_string(TimestampCaptureMethod method) noexcept;
[[nodiscard]] std::optional<TimestampCaptureMethod> parse_capture_method(std::string_view s) noexcept;

// Explicitly distinguishes real host measurements from synthetic fixtures and model estimates.
enum class ErrorSource : std::uint8_t {
    Measured = 0,          // Direct hardware/OS observation on the host
    SyntheticFixture = 1,  // Deterministic offline simulation fixture
    EstimatedModel = 2,    // Derived by an estimator model from input samples
};

[[nodiscard]] std::string_view to_string(ErrorSource source) noexcept;
[[nodiscard]] std::optional<ErrorSource> parse_error_source(std::string_view s) noexcept;

struct ClockProvenance {
    ClockDomain domain{ClockDomain::Unknown};
    TimestampRole role{TimestampRole::ReceiveTime};
    TimestampCaptureMethod capture_method{TimestampCaptureMethod::Unknown};
    ErrorSource error_source{ErrorSource::SyntheticFixture};
    std::string clock_id{};        // e.g. "gm-tai-0", "nic-eth0-phc", "host-monotonic-0"
    std::uint32_t epoch_id{0};     // Monotonically incremented on clock step or host restart
};

// A timestamp explicitly bound to its clock provenance and uncertainty half-width.
struct ProvenancedTimestamp {
    Nanos value_ns{0};
    Nanos uncertainty_ns{0};       // Non-negative half-width uncertainty bound (+/- ns)
    ClockProvenance provenance{};
};

// Two-way PTP-style synchronization exchange (Sync + Delay_Req / Delay_Resp):
//   t1_ns: Grandmaster (reference domain) transmit time of Sync
//   t2_ns: Local follower (local domain) receive time of Sync
//   t3_ns: Local follower (local domain) transmit time of Delay_Req
//   t4_ns: Grandmaster (reference domain) receive time of Delay_Resp
// Offset definition (follower minus master):
//   offset = ((t2 - t1) - (t4 - t3)) / 2
// Round-trip delay:
//   rtt = (t4 - t1) - (t3 - t2)
struct SyncExchangeSample {
    std::uint64_t seq{0};
    std::uint32_t epoch_id{0};
    ClockDomain reference_domain{ClockDomain::Tai};
    ClockDomain local_domain{ClockDomain::MonotonicRaw};
    std::string reference_clock_id{"gm-tai-0"};
    std::string local_clock_id{"local-osc-0"};
    TimestampCaptureMethod capture_method{TimestampCaptureMethod::SyntheticFixture};
    ErrorSource error_source{ErrorSource::SyntheticFixture};

    Nanos t1_ns{0};
    Nanos t2_ns{0};
    Nanos t3_ns{0};
    Nanos t4_ns{0};

    // Optional ground truth for synthetic evaluation fixtures ONLY (never used by estimators).
    std::optional<Nanos> true_offset_ns{};
    std::optional<double> true_drift_ppb{};
    std::optional<Nanos> true_asymmetry_ns{};
};

// Market-data packet carrying separated event-time and receive-time contracts.
struct TimestampedPacket {
    std::uint64_t packet_id{0};
    std::uint64_t sequence_id{0};
    std::string source_id{};
    std::string symbol{};

    ProvenancedTimestamp event_ts{};    // Authoritative event-time (role MUST be EventTime)
    ProvenancedTimestamp receive_ts{};  // Local capture time (role MUST be ReceiveTime)

    // Optional payload summary (educational/research prototype only; no real market credentials).
    std::uint32_t payload_bytes{64};
};

struct ValidationResult {
    ErrorCode code{ErrorCode::Ok};
    std::string message{};

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
    static ValidationResult success() { return {ErrorCode::Ok, {}}; }
    static ValidationResult fail(ErrorCode c, std::string msg) { return {c, std::move(msg)}; }
};

[[nodiscard]] ValidationResult validate_provenance(const ClockProvenance& p);
[[nodiscard]] ValidationResult validate_timestamp(const ProvenancedTimestamp& ts);
[[nodiscard]] ValidationResult validate_sync_sample(const SyncExchangeSample& sample);
[[nodiscard]] ValidationResult validate_packet(const TimestampedPacket& packet);

// Guard that enforces strict clock-domain matching: never silently mix domains.
[[nodiscard]] ValidationResult require_same_domain(
    const ClockProvenance& lhs,
    const ClockProvenance& rhs,
    std::string_view context
);

// Deterministic 64-bit FNV-1a fingerprint for stable run/output identifiers.
[[nodiscard]] std::uint64_t fnv1a_64(std::string_view data) noexcept;
[[nodiscard]] std::string format_hex64(std::uint64_t value);

}  // namespace ptsp
