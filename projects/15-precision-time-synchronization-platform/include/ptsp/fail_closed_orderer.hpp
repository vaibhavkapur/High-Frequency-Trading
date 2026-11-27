#pragma once

#include "ptsp/estimator.hpp"
#include "ptsp/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptsp {

enum class OrderingState : std::uint8_t {
    Healthy = 0,
    HaltedFailClosed = 1,
};

[[nodiscard]] std::string_view to_string(OrderingState state) noexcept;

enum class OrderDecision : std::uint8_t {
    Buffered = 0,
    Emitted = 1,
    RejectedDomainMismatch = 2,
    RejectedInvalidPacket = 3,
    RejectedBufferFull = 4,
    HaltedSkewViolation = 5,
    HaltedCausalViolation = 6,
    HaltedAmbiguousOrder = 7,
    RejectedWhileHalted = 8,
};

[[nodiscard]] std::string_view to_string(OrderDecision decision) noexcept;

struct OrdererConfig {
    ClockDomain expected_event_domain{ClockDomain::Tai};
    ClockDomain expected_receive_domain{ClockDomain::MonotonicRaw};
    std::size_t max_buffered_events{256};          // Bounded memory capacity
    Nanos reorder_window_ns{2'000};                // Holdback window to resolve bounded out-of-order arrivals
    Nanos max_skew_ns{5'000};                      // Maximum tolerated clock uncertainty / skew envelope
    Nanos max_causal_slack_ns{200};                // Allowed tolerance when checking aligned_receive >= event_time
    bool fail_closed_on_ambiguity{true};           // Halt fail-closed if two events have overlapping uncertainty intervals across sources
    bool require_aligned_receive_check{true};      // Validate receive-time causality against clock estimate
};

[[nodiscard]] ValidationResult validate_orderer_config(const OrdererConfig& cfg);

struct OrderedEvent {
    TimestampedPacket packet{};
    Nanos aligned_receive_time_ns{0};
    Nanos aligned_receive_uncertainty_ns{0};
    Nanos estimated_transit_latency_ns{0};         // aligned_receive_time - event_time
    Nanos combined_uncertainty_ns{0};
    bool reordered{false};                         // True if emitted out of arrival order
    std::uint64_t emit_index{0};
};

struct OrdererDiagnosticEvent {
    std::uint64_t packet_id{0};
    std::uint64_t sequence_id{0};
    OrderDecision decision{OrderDecision::Buffered};
    ErrorCode error_code{ErrorCode::Ok};
    std::string reason{};
};

struct OrdererCounters {
    std::uint64_t packets_ingested{0};
    std::uint64_t packets_emitted{0};
    std::uint64_t packets_reordered{0};
    std::uint64_t domain_mismatches{0};
    std::uint64_t invalid_packets{0};
    std::uint64_t skew_violations{0};
    std::uint64_t causal_violations{0};
    std::uint64_t ambiguity_halts{0};
    std::uint64_t buffer_overflows{0};
    std::uint64_t rejected_while_halted{0};
    std::uint64_t manual_recoveries{0};
};

// Deterministic bounded-capacity orderer with strict clock-domain enforcement,
// skew/causality verification, uncertainty-overlap ambiguity checks, and fail-closed halting.
class FailClosedOrderer {
public:
    explicit FailClosedOrderer(OrdererConfig cfg = {});

    // Ingest one packet with the current clock estimate.
    // Newly releasable events (whose event_time + uncertainty <= watermark - reorder_window)
    // are appended to `released`.
    [[nodiscard]] ValidationResult ingest(
        const TimestampedPacket& packet,
        const ClockEstimate& estimate,
        std::vector<OrderedEvent>& released
    );

    // Flush remaining buffered events at end-of-stream (refuses to flush if halted fail-closed).
    [[nodiscard]] ValidationResult flush(std::vector<OrderedEvent>& released);

    // Explicit operator recovery after investigating a fail-closed halt.
    // Clears quarantined buffer and returns state to Healthy.
    void recover_and_reset_buffer(std::string_view operator_note = "operator-reset");

    [[nodiscard]] OrderingState state() const noexcept { return state_; }
    [[nodiscard]] ErrorCode halt_reason_code() const noexcept { return halt_code_; }
    [[nodiscard]] const std::string& halt_reason_message() const noexcept { return halt_message_; }
    [[nodiscard]] std::size_t buffered_count() const noexcept { return buffer_.size(); }
    [[nodiscard]] const OrdererCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const std::vector<OrdererDiagnosticEvent>& audit_log() const noexcept { return audit_log_; }

private:
    struct BufferedEntry {
        TimestampedPacket packet{};
        Nanos aligned_receive_time_ns{0};
        Nanos aligned_receive_uncertainty_ns{0};
        Nanos transit_latency_ns{0};
        Nanos combined_uncertainty_ns{0};
        std::uint64_t arrival_index{0};
    };

    ValidationResult halt_fail_closed(
        std::uint64_t packet_id,
        std::uint64_t sequence_id,
        OrderDecision decision,
        ErrorCode code,
        std::string reason
    );

    ValidationResult drain_ready(bool flush_all, std::vector<OrderedEvent>& released);

    OrdererConfig cfg_{};
    OrderingState state_{OrderingState::Healthy};
    ErrorCode halt_code_{ErrorCode::Ok};
    std::string halt_message_{};

    std::vector<BufferedEntry> buffer_{};
    Nanos high_watermark_event_ns_{0};
    bool has_watermark_{false};
    Nanos last_emitted_event_ns_{0};
    bool has_emitted_{false};
    std::uint64_t next_arrival_index_{0};
    std::uint64_t next_emit_index_{0};
    std::uint64_t max_emitted_arrival_index_{0};

    OrdererCounters counters_{};
    std::vector<OrdererDiagnosticEvent> audit_log_{};
};

}  // namespace ptsp
