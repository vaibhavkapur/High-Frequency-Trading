#include "ptsp/fail_closed_orderer.hpp"

#include <algorithm>
#include <sstream>

namespace ptsp {

std::string_view to_string(OrderingState state) noexcept {
    switch (state) {
        case OrderingState::Healthy: return "HEALTHY";
        case OrderingState::HaltedFailClosed: return "HALTED_FAIL_CLOSED";
    }
    return "UNKNOWN";
}

std::string_view to_string(OrderDecision decision) noexcept {
    switch (decision) {
        case OrderDecision::Buffered: return "BUFFERED";
        case OrderDecision::Emitted: return "EMITTED";
        case OrderDecision::RejectedDomainMismatch: return "REJECTED_DOMAIN_MISMATCH";
        case OrderDecision::RejectedInvalidPacket: return "REJECTED_INVALID_PACKET";
        case OrderDecision::RejectedBufferFull: return "REJECTED_BUFFER_FULL";
        case OrderDecision::HaltedSkewViolation: return "HALTED_SKEW_VIOLATION";
        case OrderDecision::HaltedCausalViolation: return "HALTED_CAUSAL_VIOLATION";
        case OrderDecision::HaltedAmbiguousOrder: return "HALTED_AMBIGUOUS_ORDER";
        case OrderDecision::RejectedWhileHalted: return "REJECTED_WHILE_HALTED";
    }
    return "UNKNOWN";
}

ValidationResult validate_orderer_config(const OrdererConfig& cfg) {
    if (cfg.expected_event_domain == ClockDomain::Unknown ||
        cfg.expected_receive_domain == ClockDomain::Unknown) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "OrdererConfig clock domains must not be UNKNOWN"
        );
    }
    if (cfg.max_buffered_events == 0 || cfg.max_buffered_events > 1'000'000) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "OrdererConfig max_buffered_events must be in [1, 1000000]"
        );
    }
    if (cfg.reorder_window_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "OrdererConfig reorder_window_ns must be >= 0"
        );
    }
    if (cfg.max_skew_ns <= 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "OrdererConfig max_skew_ns must be > 0"
        );
    }
    if (cfg.max_causal_slack_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "OrdererConfig max_causal_slack_ns must be >= 0"
        );
    }
    return ValidationResult::success();
}

FailClosedOrderer::FailClosedOrderer(OrdererConfig cfg) : cfg_(cfg) {
    buffer_.reserve(std::min<std::size_t>(cfg_.max_buffered_events, 4096));
}

ValidationResult FailClosedOrderer::halt_fail_closed(
    std::uint64_t packet_id,
    std::uint64_t sequence_id,
    OrderDecision decision,
    ErrorCode code,
    std::string reason
) {
    state_ = OrderingState::HaltedFailClosed;
    halt_code_ = code;
    halt_message_ = reason;
    audit_log_.push_back(OrdererDiagnosticEvent{
        packet_id,
        sequence_id,
        decision,
        code,
        reason
    });
    return ValidationResult::fail(code, std::move(reason));
}

void FailClosedOrderer::recover_and_reset_buffer(std::string_view operator_note) {
    buffer_.clear();
    state_ = OrderingState::Healthy;
    halt_code_ = ErrorCode::Ok;
    halt_message_.clear();
    has_watermark_ = false;
    has_emitted_ = false;
    ++counters_.manual_recoveries;
    audit_log_.push_back(OrdererDiagnosticEvent{
        0,
        0,
        OrderDecision::Buffered,
        ErrorCode::Ok,
        "Manual fail-closed recovery: " + std::string(operator_note)
    });
}

ValidationResult FailClosedOrderer::ingest(
    const TimestampedPacket& packet,
    const ClockEstimate& estimate,
    std::vector<OrderedEvent>& released
) {
    ++counters_.packets_ingested;

    if (state_ == OrderingState::HaltedFailClosed) {
        ++counters_.rejected_while_halted;
        audit_log_.push_back(OrdererDiagnosticEvent{
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::RejectedWhileHalted,
            ErrorCode::FailClosedHalted,
            "Orderer is halted fail-closed: " + halt_message_
        });
        return ValidationResult::fail(
            ErrorCode::FailClosedHalted,
            "Orderer is halted fail-closed (" + halt_message_ + "); operator recovery required"
        );
    }

    // 1) Validate packet contract (event-time vs receive-time roles & bounds).
    auto pkt_valid = validate_packet(packet);
    if (!pkt_valid.ok()) {
        ++counters_.invalid_packets;
        audit_log_.push_back(OrdererDiagnosticEvent{
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::RejectedInvalidPacket,
            pkt_valid.code,
            pkt_valid.message
        });
        return pkt_valid;
    }

    // 2) Strictly enforce clock domains: NEVER silently mix domains.
    if (packet.event_ts.provenance.domain != cfg_.expected_event_domain) {
        ++counters_.domain_mismatches;
        std::ostringstream oss;
        oss << "Event clock domain mismatch: expected " << to_string(cfg_.expected_event_domain)
            << " but packet " << packet.packet_id << " carries "
            << to_string(packet.event_ts.provenance.domain);
        audit_log_.push_back(OrdererDiagnosticEvent{
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::RejectedDomainMismatch,
            ErrorCode::ClockDomainMismatch,
            oss.str()
        });
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }

    if (packet.receive_ts.provenance.domain != cfg_.expected_receive_domain) {
        ++counters_.domain_mismatches;
        std::ostringstream oss;
        oss << "Receive clock domain mismatch: expected " << to_string(cfg_.expected_receive_domain)
            << " but packet " << packet.packet_id << " carries "
            << to_string(packet.receive_ts.provenance.domain);
        audit_log_.push_back(OrdererDiagnosticEvent{
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::RejectedDomainMismatch,
            ErrorCode::ClockDomainMismatch,
            oss.str()
        });
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }

    // 3) Validate estimator alignment & skew envelope.
    Nanos aligned_rx_ns = packet.receive_ts.value_ns;
    Nanos aligned_rx_unc = packet.receive_ts.uncertainty_ns;
    Nanos combined_unc = packet.event_ts.uncertainty_ns + packet.receive_ts.uncertainty_ns;

    if (cfg_.require_aligned_receive_check) {
        if (!estimate.valid) {
            ++counters_.skew_violations;
            return halt_fail_closed(
                packet.packet_id,
                packet.sequence_id,
                OrderDecision::HaltedSkewViolation,
                ErrorCode::SkewViolation,
                "No valid ClockEstimate available to align receive_ts into event domain"
            );
        }
        if (estimate.reference_domain != cfg_.expected_event_domain ||
            estimate.local_domain != cfg_.expected_receive_domain) {
            ++counters_.domain_mismatches;
            std::ostringstream oss;
            oss << "Estimator domain pair (" << to_string(estimate.reference_domain) << ", "
                << to_string(estimate.local_domain) << ") does not match orderer domains ("
                << to_string(cfg_.expected_event_domain) << ", "
                << to_string(cfg_.expected_receive_domain) << ")";
            audit_log_.push_back(OrdererDiagnosticEvent{
                packet.packet_id,
                packet.sequence_id,
                OrderDecision::RejectedDomainMismatch,
                ErrorCode::ClockDomainMismatch,
                oss.str()
            });
            return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
        }

        ProvenancedTimestamp aligned_rx{};
        auto align_res = align_local_to_reference(packet.receive_ts, estimate, aligned_rx);
        if (!align_res.ok()) {
            ++counters_.domain_mismatches;
            return align_res;
        }
        aligned_rx_ns = aligned_rx.value_ns;
        aligned_rx_unc = aligned_rx.uncertainty_ns;
        combined_unc = packet.event_ts.uncertainty_ns + aligned_rx_unc;

        // Check skew / uncertainty envelope violation.
        if (estimate.total_uncertainty_ns > cfg_.max_skew_ns || combined_unc > cfg_.max_skew_ns) {
            ++counters_.skew_violations;
            std::ostringstream oss;
            oss << "Skew/uncertainty envelope exceeded on packet " << packet.packet_id
                << ": estimator_uncertainty_ns=" << estimate.total_uncertainty_ns
                << ", combined_uncertainty_ns=" << combined_unc
                << " > max_skew_ns=" << cfg_.max_skew_ns;
            return halt_fail_closed(
                packet.packet_id,
                packet.sequence_id,
                OrderDecision::HaltedSkewViolation,
                ErrorCode::SkewViolation,
                oss.str()
            );
        }

        // Check receive-time vs event-time causality:
        // A packet cannot be received before the event occurred (beyond combined uncertainty + causal slack).
        const Nanos transit_latency_ns = aligned_rx_ns - packet.event_ts.value_ns;
        if (transit_latency_ns + combined_unc + cfg_.max_causal_slack_ns < 0) {
            ++counters_.causal_violations;
            std::ostringstream oss;
            oss << "Causal skew violation on packet " << packet.packet_id
                << ": aligned_receive_ns=" << aligned_rx_ns
                << " precedes event_time_ns=" << packet.event_ts.value_ns
                << " by " << (-transit_latency_ns) << " ns (exceeds uncertainty "
                << combined_unc << " + slack " << cfg_.max_causal_slack_ns << " ns)";
            return halt_fail_closed(
                packet.packet_id,
                packet.sequence_id,
                OrderDecision::HaltedCausalViolation,
                ErrorCode::CausalViolation,
                oss.str()
            );
        }
    }

    // 4) Check late arrival past already-emitted event watermark (ordering invariant violation -> fail closed).
    if (has_emitted_ && packet.event_ts.value_ns < last_emitted_event_ns_) {
        ++counters_.skew_violations;
        std::ostringstream oss;
        oss << "Late out-of-window packet " << packet.packet_id
            << " with event_time_ns=" << packet.event_ts.value_ns
            << " arrived after already-emitted event_time_ns=" << last_emitted_event_ns_;
        return halt_fail_closed(
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::HaltedSkewViolation,
            ErrorCode::SkewViolation,
            oss.str()
        );
    }

    // 5) Enforce bounded capacity strictly (never silently drop or allocate unboundedly).
    if (buffer_.size() >= cfg_.max_buffered_events) {
        ++counters_.buffer_overflows;
        std::ostringstream oss;
        oss << "Bounded reorder buffer full (capacity=" << cfg_.max_buffered_events
            << ") on packet " << packet.packet_id;
        audit_log_.push_back(OrdererDiagnosticEvent{
            packet.packet_id,
            packet.sequence_id,
            OrderDecision::RejectedBufferFull,
            ErrorCode::BufferCapacityExceeded,
            oss.str()
        });
        return ValidationResult::fail(ErrorCode::BufferCapacityExceeded, oss.str());
    }

    const Nanos transit_ns = aligned_rx_ns - packet.event_ts.value_ns;
    buffer_.push_back(BufferedEntry{
        packet,
        aligned_rx_ns,
        aligned_rx_unc,
        transit_ns,
        combined_unc,
        next_arrival_index_++
    });

    if (!has_watermark_ || packet.event_ts.value_ns > high_watermark_event_ns_) {
        high_watermark_event_ns_ = packet.event_ts.value_ns;
        has_watermark_ = true;
    }

    return drain_ready(false, released);
}

ValidationResult FailClosedOrderer::flush(std::vector<OrderedEvent>& released) {
    if (state_ == OrderingState::HaltedFailClosed) {
        return ValidationResult::fail(
            ErrorCode::FailClosedHalted,
            "Cannot flush while orderer is halted fail-closed: " + halt_message_
        );
    }
    return drain_ready(true, released);
}

ValidationResult FailClosedOrderer::drain_ready(bool flush_all, std::vector<OrderedEvent>& released) {
    if (buffer_.empty()) {
        return ValidationResult::success();
    }

    // Deterministic sort by (event_time_ns, source_id, sequence_id, packet_id).
    std::sort(buffer_.begin(), buffer_.end(), [](const BufferedEntry& a, const BufferedEntry& b) {
        if (a.packet.event_ts.value_ns != b.packet.event_ts.value_ns) {
            return a.packet.event_ts.value_ns < b.packet.event_ts.value_ns;
        }
        if (a.packet.source_id != b.packet.source_id) {
            return a.packet.source_id < b.packet.source_id;
        }
        if (a.packet.sequence_id != b.packet.sequence_id) {
            return a.packet.sequence_id < b.packet.sequence_id;
        }
        return a.packet.packet_id < b.packet.packet_id;
    });

    const Nanos release_cutoff = high_watermark_event_ns_ - cfg_.reorder_window_ns;
    std::size_t ready_count = 0;
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
        if (flush_all || buffer_[i].packet.event_ts.value_ns <= release_cutoff) {
            ready_count = i + 1;
        } else {
            break;
        }
    }

    if (ready_count == 0) {
        return ValidationResult::success();
    }

    // Fail-closed ambiguity check: if two adjacent events from DIFFERENT sources have overlapping
    // event-time uncertainty intervals [t - u, t + u], their true causal ordering is undecidable.
    if (cfg_.fail_closed_on_ambiguity) {
        const std::size_t check_limit = std::min(ready_count + 1, buffer_.size());
        for (std::size_t i = 0; i + 1 < check_limit; ++i) {
            const auto& a = buffer_[i];
            const auto& b = buffer_[i + 1];
            if (a.packet.source_id == b.packet.source_id) {
                continue;  // Same source is totally ordered by sequence_id
            }
            const Nanos a_upper = a.packet.event_ts.value_ns + a.packet.event_ts.uncertainty_ns;
            const Nanos b_lower = b.packet.event_ts.value_ns - b.packet.event_ts.uncertainty_ns;
            if (a_upper >= b_lower) {
                ++counters_.ambiguity_halts;
                std::ostringstream oss;
                oss << "Ambiguous cross-source causal ordering between packet " << a.packet.packet_id
                    << " (" << a.packet.source_id << ", event_ns=" << a.packet.event_ts.value_ns
                    << " +/- " << a.packet.event_ts.uncertainty_ns << ") and packet " << b.packet.packet_id
                    << " (" << b.packet.source_id << ", event_ns=" << b.packet.event_ts.value_ns
                    << " +/- " << b.packet.event_ts.uncertainty_ns << ")";
                return halt_fail_closed(
                    b.packet.packet_id,
                    b.packet.sequence_id,
                    OrderDecision::HaltedAmbiguousOrder,
                    ErrorCode::AmbiguousOrdering,
                    oss.str()
                );
            }
        }
    }

    for (std::size_t i = 0; i < ready_count; ++i) {
        const auto& e = buffer_[i];
        const bool reordered = (next_emit_index_ > 0) && (e.arrival_index < max_emitted_arrival_index_);
        if (e.arrival_index > max_emitted_arrival_index_) {
            max_emitted_arrival_index_ = e.arrival_index;
        }
        if (reordered) {
            ++counters_.packets_reordered;
        }
        OrderedEvent ev{};
        ev.packet = e.packet;
        ev.aligned_receive_time_ns = e.aligned_receive_time_ns;
        ev.aligned_receive_uncertainty_ns = e.aligned_receive_uncertainty_ns;
        ev.estimated_transit_latency_ns = e.transit_latency_ns;
        ev.combined_uncertainty_ns = e.combined_uncertainty_ns;
        ev.reordered = reordered;
        ev.emit_index = next_emit_index_++;
        released.push_back(std::move(ev));

        last_emitted_event_ns_ = e.packet.event_ts.value_ns;
        has_emitted_ = true;
        ++counters_.packets_emitted;
    }

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(ready_count));
    return ValidationResult::success();
}

}  // namespace ptsp
