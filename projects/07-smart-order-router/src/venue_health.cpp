#include "sor/venue_health.hpp"

#include <algorithm>

namespace sor {

void VenueHealthMonitor::reset() noexcept {
    for (auto& v : venues_) {
        v = VenueHealthSnapshot{};
    }
}

void VenueHealthMonitor::reset_venue(VenueId vid) noexcept {
    if (vid >= kMaxVenues) return;
    venues_[vid] = VenueHealthSnapshot{};
}

void VenueHealthMonitor::record_ack(VenueId vid, TimestampNs rtt_ns,
                                    TimestampNs /*now_ns*/) noexcept {
    if (vid >= kMaxVenues) return;
    auto& v = venues_[vid];
    ++v.total_acks;
    v.consecutive_rejects = 0;
    v.consecutive_timeouts = 0;
    if (!v.manually_disabled && v.state != VenueHealthState::Healthy) {
        v.state = VenueHealthState::Healthy;
    }

    if (rtt_ns > 0) {
        const double one_way = 0.5 * static_cast<double>(rtt_ns);
        if (!v.has_latency_sample) {
            v.ewma_one_way_latency_ns = one_way;
            v.has_latency_sample = true;
        } else {
            v.ewma_one_way_latency_ns =
                cfg_.latency_ewma_alpha * one_way +
                (1.0 - cfg_.latency_ewma_alpha) * v.ewma_one_way_latency_ns;
        }
    }
}

void VenueHealthMonitor::record_fill_outcome(VenueId vid, Quantity requested_qty,
                                             Quantity filled_qty) noexcept {
    if (vid >= kMaxVenues || requested_qty <= 0) return;
    auto& v = venues_[vid];
    const double ratio = std::clamp(
        static_cast<double>(filled_qty) / static_cast<double>(requested_qty), 0.0, 1.0);
    if (!v.has_fill_sample) {
        v.ewma_fill_ratio = ratio;
        v.has_fill_sample = true;
    } else {
        v.ewma_fill_ratio =
            cfg_.fill_prob_ewma_alpha * ratio + (1.0 - cfg_.fill_prob_ewma_alpha) * v.ewma_fill_ratio;
    }
}

bool VenueHealthMonitor::record_reject(VenueId vid, TimestampNs now_ns) noexcept {
    if (vid >= kMaxVenues) return false;
    auto& v = venues_[vid];
    ++v.consecutive_rejects;
    ++v.total_rejects;

    const VenueHealthState prev = v.state;
    if (v.consecutive_rejects >= cfg_.circuit_open_reject_threshold) {
        v.state = VenueHealthState::CircuitOpen;
        v.circuit_opened_at_ns = now_ns;
        v.reopen_after_ns =
            cfg_.recovery_cooldown_ns > 0 ? (now_ns + cfg_.recovery_cooldown_ns) : 0;
    } else if (v.consecutive_rejects >= cfg_.degraded_reject_threshold &&
               v.state == VenueHealthState::Healthy) {
        v.state = VenueHealthState::Degraded;
    }
    return prev != VenueHealthState::CircuitOpen && v.state == VenueHealthState::CircuitOpen;
}

bool VenueHealthMonitor::record_timeout(VenueId vid, TimestampNs now_ns) noexcept {
    if (vid >= kMaxVenues) return false;
    auto& v = venues_[vid];
    ++v.consecutive_timeouts;
    ++v.total_timeouts;

    const VenueHealthState prev = v.state;
    if (v.consecutive_timeouts >= cfg_.circuit_open_timeout_threshold) {
        v.state = VenueHealthState::CircuitOpen;
        v.circuit_opened_at_ns = now_ns;
        v.reopen_after_ns =
            cfg_.recovery_cooldown_ns > 0 ? (now_ns + cfg_.recovery_cooldown_ns) : 0;
    } else if (v.state == VenueHealthState::Healthy) {
        v.state = VenueHealthState::Degraded;
    }
    return prev != VenueHealthState::CircuitOpen && v.state == VenueHealthState::CircuitOpen;
}

void VenueHealthMonitor::set_manual_down(VenueId vid, bool down, TimestampNs now_ns) noexcept {
    if (vid >= kMaxVenues) return;
    auto& v = venues_[vid];
    v.manually_disabled = down;
    if (down) {
        v.state = VenueHealthState::CircuitOpen;
        v.circuit_opened_at_ns = now_ns;
        v.reopen_after_ns = 0;  // Stays closed until manually cleared.
    } else {
        v.state = VenueHealthState::Healthy;
        v.consecutive_rejects = 0;
        v.consecutive_timeouts = 0;
    }
}

VenueHealthState VenueHealthMonitor::state(VenueId vid, TimestampNs now_ns) const noexcept {
    if (vid >= kMaxVenues) return VenueHealthState::CircuitOpen;
    const auto& v = venues_[vid];
    if (v.manually_disabled) return VenueHealthState::CircuitOpen;
    if (v.state == VenueHealthState::CircuitOpen && v.reopen_after_ns > 0 &&
        now_ns >= v.reopen_after_ns) {
        return VenueHealthState::Degraded;
    }
    return v.state;
}

bool VenueHealthMonitor::is_routable(VenueId vid, TimestampNs now_ns) const noexcept {
    return state(vid, now_ns) != VenueHealthState::CircuitOpen;
}

double VenueHealthMonitor::estimated_one_way_latency_ns(VenueId vid,
                                                        const VenueConfig& vcfg) const noexcept {
    const double base = static_cast<double>(vcfg.base_one_way_latency_ns);
    if (vid >= kMaxVenues || !venues_[vid].has_latency_sample) {
        return base;
    }
    // Blend 50% prior config latency + 50% observed EWMA so estimator stays bounded yet adaptive.
    return 0.5 * base + 0.5 * venues_[vid].ewma_one_way_latency_ns;
}

double VenueHealthMonitor::fill_prob_adjustment(VenueId vid,
                                                const VenueConfig& vcfg) const noexcept {
    if (vid >= kMaxVenues || !venues_[vid].has_fill_sample) {
        return 1.0;
    }
    const double prior = std::max(0.05, vcfg.base_fill_prob);
    const double observed = std::clamp(venues_[vid].ewma_fill_ratio, 0.05, 1.0);
    // Dampened multiplier around 1.0 so transient misses shift preference smoothly.
    const double raw_mult = observed / prior;
    return std::clamp(0.5 + 0.5 * raw_mult, 0.25, 1.15);
}

}  // namespace sor
