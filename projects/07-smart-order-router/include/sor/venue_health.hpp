#pragma once

#include <array>
#include <cstdint>

#include "sor/types.hpp"

namespace sor {

struct VenueHealthConfig {
    std::uint32_t degraded_reject_threshold{1};
    std::uint32_t circuit_open_reject_threshold{3};
    std::uint32_t circuit_open_timeout_threshold{2};
    TimestampNs recovery_cooldown_ns{5'000'000'000};  // 5 seconds
    double latency_ewma_alpha{0.25};
    double fill_prob_ewma_alpha{0.20};
};

struct VenueHealthSnapshot {
    VenueHealthState state{VenueHealthState::Healthy};
    bool manually_disabled{false};
    std::uint32_t consecutive_rejects{0};
    std::uint32_t consecutive_timeouts{0};
    std::uint64_t total_rejects{0};
    std::uint64_t total_timeouts{0};
    std::uint64_t total_acks{0};
    double ewma_one_way_latency_ns{0.0};
    bool has_latency_sample{false};
    double ewma_fill_ratio{1.0};
    bool has_fill_sample{false};
    TimestampNs circuit_opened_at_ns{0};
    TimestampNs reopen_after_ns{0};
};

class VenueHealthMonitor {
public:
    explicit VenueHealthMonitor(VenueHealthConfig cfg = {}) noexcept : cfg_(cfg) {}

    void configure(VenueHealthConfig cfg) noexcept { cfg_ = cfg; }
    [[nodiscard]] const VenueHealthConfig& config() const noexcept { return cfg_; }

    void reset() noexcept;
    void reset_venue(VenueId vid) noexcept;

    // Record successful ack with observed round-trip time.
    void record_ack(VenueId vid, TimestampNs rtt_ns, TimestampNs now_ns) noexcept;

    // Record terminal child outcome (filled_qty / requested_qty) to adapt fill-probability estimate.
    void record_fill_outcome(VenueId vid, Quantity requested_qty, Quantity filled_qty) noexcept;

    // Record order reject or response timeout. Returns true if state transitioned to CircuitOpen.
    bool record_reject(VenueId vid, TimestampNs now_ns) noexcept;
    bool record_timeout(VenueId vid, TimestampNs now_ns) noexcept;

    // Administrative kill switch / scripted venue outage control.
    void set_manual_down(VenueId vid, bool down, TimestampNs now_ns) noexcept;

    // Check whether venue is eligible to receive new child orders at `now_ns`.
    [[nodiscard]] bool is_routable(VenueId vid, TimestampNs now_ns) const noexcept;
    [[nodiscard]] VenueHealthState state(VenueId vid, TimestampNs now_ns) const noexcept;
    [[nodiscard]] const VenueHealthSnapshot& snapshot(VenueId vid) const noexcept {
        return venues_[vid];
    }

    // Estimated one-way latency combining configured baseline and observed EWMA RTT / 2.
    [[nodiscard]] double estimated_one_way_latency_ns(VenueId vid,
                                                      const VenueConfig& vcfg) const noexcept;

    // Multiplicative adjustment to VenueConfig::base_fill_prob based on observed fill ratio.
    [[nodiscard]] double fill_prob_adjustment(VenueId vid, const VenueConfig& vcfg) const noexcept;

private:
    VenueHealthConfig cfg_{};
    std::array<VenueHealthSnapshot, kMaxVenues> venues_{};
};

}  // namespace sor
