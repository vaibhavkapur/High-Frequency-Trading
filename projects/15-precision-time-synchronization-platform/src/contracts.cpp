#include "ptsp/types.hpp"

#include <iomanip>
#include <sstream>

namespace ptsp {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::InvalidConfig: return "INVALID_CONFIG";
        case ErrorCode::MissingConfig: return "MISSING_CONFIG";
        case ErrorCode::InvalidTimestamp: return "INVALID_TIMESTAMP";
        case ErrorCode::ClockDomainMismatch: return "CLOCK_DOMAIN_MISMATCH";
        case ErrorCode::UnsupportedClockDomain: return "UNSUPPORTED_CLOCK_DOMAIN";
        case ErrorCode::InvalidSyncSample: return "INVALID_SYNC_SAMPLE";
        case ErrorCode::NonMonotonicCounter: return "NON_MONOTONIC_COUNTER";
        case ErrorCode::HardwareTimestampUnavailable: return "HARDWARE_TIMESTAMP_UNAVAILABLE";
        case ErrorCode::UnsupportedPlatform: return "UNSUPPORTED_PLATFORM";
        case ErrorCode::SkewViolation: return "SKEW_VIOLATION";
        case ErrorCode::CausalViolation: return "CAUSAL_VIOLATION";
        case ErrorCode::AmbiguousOrdering: return "AMBIGUOUS_ORDERING";
        case ErrorCode::BufferCapacityExceeded: return "BUFFER_CAPACITY_EXCEEDED";
        case ErrorCode::FailClosedHalted: return "FAIL_CLOSED_HALTED";
        case ErrorCode::IoError: return "IO_ERROR";
        case ErrorCode::ParseError: return "PARSE_ERROR";
        case ErrorCode::SchemaVersionMismatch: return "SCHEMA_VERSION_MISMATCH";
    }
    return "UNKNOWN_ERROR";
}

std::string_view to_string(ClockDomain domain) noexcept {
    switch (domain) {
        case ClockDomain::Unknown: return "UNKNOWN";
        case ClockDomain::Tai: return "TAI";
        case ClockDomain::Utc: return "UTC";
        case ClockDomain::MonotonicRaw: return "MONOTONIC_RAW";
        case ClockDomain::Monotonic: return "MONOTONIC";
        case ClockDomain::ExchangeVenue: return "EXCHANGE_VENUE";
    }
    return "UNKNOWN";
}

std::optional<ClockDomain> parse_clock_domain(std::string_view s) noexcept {
    if (s == "TAI") return ClockDomain::Tai;
    if (s == "UTC") return ClockDomain::Utc;
    if (s == "MONOTONIC_RAW") return ClockDomain::MonotonicRaw;
    if (s == "MONOTONIC") return ClockDomain::Monotonic;
    if (s == "EXCHANGE_VENUE") return ClockDomain::ExchangeVenue;
    if (s == "UNKNOWN") return ClockDomain::Unknown;
    return std::nullopt;
}

std::string_view to_string(TimestampRole role) noexcept {
    switch (role) {
        case TimestampRole::EventTime: return "EVENT_TIME";
        case TimestampRole::ReceiveTime: return "RECEIVE_TIME";
        case TimestampRole::TransmitTime: return "TRANSMIT_TIME";
    }
    return "UNKNOWN_ROLE";
}

std::optional<TimestampRole> parse_timestamp_role(std::string_view s) noexcept {
    if (s == "EVENT_TIME") return TimestampRole::EventTime;
    if (s == "RECEIVE_TIME") return TimestampRole::ReceiveTime;
    if (s == "TRANSMIT_TIME") return TimestampRole::TransmitTime;
    return std::nullopt;
}

std::string_view to_string(TimestampCaptureMethod method) noexcept {
    switch (method) {
        case TimestampCaptureMethod::Unknown: return "UNKNOWN";
        case TimestampCaptureMethod::HardwareNicPtp: return "HARDWARE_NIC_PTP";
        case TimestampCaptureMethod::KernelSoftware: return "KERNEL_SOFTWARE";
        case TimestampCaptureMethod::UserspaceClock: return "USERSPACE_CLOCK";
        case TimestampCaptureMethod::SyntheticFixture: return "SYNTHETIC_FIXTURE";
        case TimestampCaptureMethod::RecordedReplay: return "RECORDED_REPLAY";
    }
    return "UNKNOWN";
}

std::optional<TimestampCaptureMethod> parse_capture_method(std::string_view s) noexcept {
    if (s == "HARDWARE_NIC_PTP") return TimestampCaptureMethod::HardwareNicPtp;
    if (s == "KERNEL_SOFTWARE") return TimestampCaptureMethod::KernelSoftware;
    if (s == "USERSPACE_CLOCK") return TimestampCaptureMethod::UserspaceClock;
    if (s == "SYNTHETIC_FIXTURE") return TimestampCaptureMethod::SyntheticFixture;
    if (s == "RECORDED_REPLAY") return TimestampCaptureMethod::RecordedReplay;
    if (s == "UNKNOWN") return TimestampCaptureMethod::Unknown;
    return std::nullopt;
}

std::string_view to_string(ErrorSource source) noexcept {
    switch (source) {
        case ErrorSource::Measured: return "MEASURED";
        case ErrorSource::SyntheticFixture: return "SYNTHETIC_FIXTURE";
        case ErrorSource::EstimatedModel: return "ESTIMATED_MODEL";
    }
    return "UNKNOWN";
}

std::optional<ErrorSource> parse_error_source(std::string_view s) noexcept {
    if (s == "MEASURED") return ErrorSource::Measured;
    if (s == "SYNTHETIC_FIXTURE") return ErrorSource::SyntheticFixture;
    if (s == "ESTIMATED_MODEL") return ErrorSource::EstimatedModel;
    return std::nullopt;
}

ValidationResult validate_provenance(const ClockProvenance& p) {
    if (p.domain == ClockDomain::Unknown) {
        return ValidationResult::fail(
            ErrorCode::UnsupportedClockDomain,
            "Clock provenance domain must not be UNKNOWN"
        );
    }
    if (p.capture_method == TimestampCaptureMethod::Unknown) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "Clock provenance capture_method must not be UNKNOWN"
        );
    }
    if (p.clock_id.empty()) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "Clock provenance clock_id must be non-empty"
        );
    }
    return ValidationResult::success();
}

ValidationResult validate_timestamp(const ProvenancedTimestamp& ts) {
    auto prov = validate_provenance(ts.provenance);
    if (!prov.ok()) {
        return prov;
    }
    if (ts.value_ns <= 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "Timestamp value_ns must be strictly positive"
        );
    }
    if (ts.uncertainty_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "Timestamp uncertainty_ns must be non-negative"
        );
    }
    return ValidationResult::success();
}

ValidationResult validate_sync_sample(const SyncExchangeSample& sample) {
    if (sample.reference_domain == ClockDomain::Unknown || sample.local_domain == ClockDomain::Unknown) {
        return ValidationResult::fail(
            ErrorCode::UnsupportedClockDomain,
            "SyncExchangeSample reference_domain and local_domain must be specified"
        );
    }
    if (sample.reference_clock_id.empty() || sample.local_clock_id.empty()) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample clock IDs must be non-empty"
        );
    }
    if (sample.capture_method == TimestampCaptureMethod::Unknown) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample capture_method must not be UNKNOWN"
        );
    }
    if (sample.t1_ns <= 0 || sample.t2_ns <= 0 || sample.t3_ns <= 0 || sample.t4_ns <= 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample timestamps (t1..t4) must be strictly positive"
        );
    }
    if (sample.t4_ns < sample.t1_ns) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample has t4_ns < t1_ns (negative reference span)"
        );
    }
    if (sample.t3_ns < sample.t2_ns) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample has t3_ns < t2_ns (negative local turnaround)"
        );
    }
    const Nanos rtt_ns = (sample.t4_ns - sample.t1_ns) - (sample.t3_ns - sample.t2_ns);
    if (rtt_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidSyncSample,
            "SyncExchangeSample implies negative round-trip delay"
        );
    }
    return ValidationResult::success();
}

ValidationResult validate_packet(const TimestampedPacket& packet) {
    if (packet.source_id.empty()) {
        return ValidationResult::fail(ErrorCode::InvalidTimestamp, "Packet source_id must be non-empty");
    }
    if (packet.symbol.empty()) {
        return ValidationResult::fail(ErrorCode::InvalidTimestamp, "Packet symbol must be non-empty");
    }
    auto ev_res = validate_timestamp(packet.event_ts);
    if (!ev_res.ok()) {
        return ValidationResult::fail(ev_res.code, "event_ts: " + ev_res.message);
    }
    if (packet.event_ts.provenance.role != TimestampRole::EventTime) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "event_ts provenance role must be EVENT_TIME"
        );
    }
    auto rx_res = validate_timestamp(packet.receive_ts);
    if (!rx_res.ok()) {
        return ValidationResult::fail(rx_res.code, "receive_ts: " + rx_res.message);
    }
    if (packet.receive_ts.provenance.role != TimestampRole::ReceiveTime) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "receive_ts provenance role must be RECEIVE_TIME"
        );
    }
    return ValidationResult::success();
}

ValidationResult require_same_domain(
    const ClockProvenance& lhs,
    const ClockProvenance& rhs,
    std::string_view context
) {
    if (lhs.domain != rhs.domain) {
        std::ostringstream oss;
        oss << "Clock domain mismatch in " << context << ": "
            << to_string(lhs.domain) << " (" << lhs.clock_id << ") vs "
            << to_string(rhs.domain) << " (" << rhs.clock_id << ")";
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }
    return ValidationResult::success();
}

std::uint64_t fnv1a_64(std::string_view data) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffsetBasis;
    for (char ch : data) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= kPrime;
    }
    return hash;
}

std::string format_hex64(std::uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << value;
    return oss.str();
}

}  // namespace ptsp
