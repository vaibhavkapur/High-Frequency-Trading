#pragma once

#include "ptsp/ring_buffer.hpp"
#include "ptsp/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptsp {

struct EstimatorConfig {
    std::size_t window_size{32};               // Bounded sliding window (<= 128)
    double outlier_mad_multiplier{3.5};        // Robust MAD outlier rejection threshold
    Nanos outlier_min_mad_ns{25};              // Floor for MAD to avoid over-rejecting ultra-clean clocks
    double rtt_spike_factor{1.75};             // Reject samples whose RTT exceeds factor * min_rtt + rtt_slack_ns
    Nanos rtt_slack_ns{250};                   // Additive slack for small RTTs
    Nanos step_threshold_ns{5'000};            // Residual jump magnitude indicating a clock step
    std::size_t step_confirm_count{2};         // Consecutive jumps required to confirm step vs outlier
    Nanos base_measurement_noise_ns{20};       // Instrument resolution / floor uncertainty
    double huber_delta_ns{150.0};              // Huber loss transition for IRLS slope/intercept
    std::size_t irls_iterations{4};            // Bounded IRLS iterations for deterministic hot-path latency
};

[[nodiscard]] ValidationResult validate_estimator_config(const EstimatorConfig& cfg);

struct ClockEstimate {
    bool valid{false};
    std::uint64_t seq{0};
    std::uint32_t estimator_epoch{0};

    ClockDomain reference_domain{ClockDomain::Unknown};
    ClockDomain local_domain{ClockDomain::Unknown};
    std::string reference_clock_id{};
    std::string local_clock_id{};
    ErrorSource error_source{ErrorSource::EstimatedModel};
    ErrorSource input_error_source{ErrorSource::SyntheticFixture};

    // Anchor point in reference domain (nanoseconds) for linear model:
    //   offset(t_ref) = offset_ns + drift_ppb * 1e-9 * (t_ref - anchor_ref_ns)
    Nanos anchor_ref_ns{0};
    Nanos offset_ns{0};                 // Estimated local - reference offset at anchor_ref_ns
    double drift_ppb{0.0};              // Estimated clock frequency offset in parts-per-billion (ns/s)

    // Explicit uncertainty decomposition (all >= 0, in ns or ppb).
    Nanos residual_std_ns{0};           // Dispersion of inlier residuals
    Nanos asymmetry_bound_ns{0};        // Half of excess RTT above minimum observed path delay
    Nanos measurement_noise_ns{0};      // Instrument noise floor
    Nanos total_uncertainty_ns{0};      // Conservative combined half-width (+/- ns)
    double drift_uncertainty_ppb{0.0};

    // Event flags for this update.
    bool sample_rejected_outlier{false};
    bool step_detected{false};
    bool restart_detected{false};
    Nanos raw_offset_ns{0};
    Nanos raw_rtt_ns{0};
    Nanos min_rtt_ns{0};
};

struct EstimatorDiagnostics {
    std::uint64_t samples_processed{0};
    std::uint64_t samples_accepted{0};
    std::uint64_t outliers_rejected{0};
    std::uint64_t steps_detected{0};
    std::uint64_t restarts_detected{0};
    std::uint64_t domain_errors{0};
    std::uint64_t validation_errors{0};
};

// Helper to project a local timestamp into the reference domain using a valid ClockEstimate:
//   t_ref = t_local - offset(t_local)
// Returns InvalidTimestamp if estimate is invalid or domains do not match.
[[nodiscard]] ValidationResult align_local_to_reference(
    const ProvenancedTimestamp& local_ts,
    const ClockEstimate& estimate,
    ProvenancedTimestamp& out_aligned
);

// Baseline unweighted sliding-window OLS estimator (no outlier rejection, no step/restart detection).
// Provided for like-for-like comparison against RobustSyncEstimator on identical event streams.
class BaselineOlsEstimator {
public:
    static constexpr std::size_t kMaxWindow = 128;

    explicit BaselineOlsEstimator(EstimatorConfig cfg = {});

    [[nodiscard]] ValidationResult update(const SyncExchangeSample& sample, ClockEstimate& out_estimate);
    [[nodiscard]] const ClockEstimate& last_estimate() const noexcept { return last_estimate_; }
    [[nodiscard]] const EstimatorDiagnostics& diagnostics() const noexcept { return diag_; }
    void reset() noexcept;

private:
    struct Point {
        Nanos t_ref_ns{0};
        Nanos offset_ns{0};
        Nanos rtt_ns{0};
    };

    EstimatorConfig cfg_{};
    BoundedRingBuffer<Point, kMaxWindow> window_{};
    ClockDomain locked_ref_domain_{ClockDomain::Unknown};
    ClockDomain locked_local_domain_{ClockDomain::Unknown};
    Nanos min_rtt_ns_{0};
    bool has_min_rtt_{false};
    ClockEstimate last_estimate_{};
    EstimatorDiagnostics diag_{};
};

// Production-grade prototype robust estimator:
// - Strict clock-domain lock (refuses silent cross-domain mixing)
// - Restart detection (epoch change or non-monotonic local t2/t3 counters)
// - Consecutive-sample clock step detection (separates true steps from single-packet outliers)
// - Minimum-RTT / MAD outlier filtering for asymmetric delay spikes
// - Bounded IRLS Huber regression for drift (ppb) and offset (ns)
// - Conservative uncertainty envelope separating residual dispersion, asymmetry bound, and noise floor
class RobustSyncEstimator {
public:
    static constexpr std::size_t kMaxWindow = 128;

    explicit RobustSyncEstimator(EstimatorConfig cfg = {});

    [[nodiscard]] ValidationResult update(const SyncExchangeSample& sample, ClockEstimate& out_estimate);
    [[nodiscard]] const ClockEstimate& last_estimate() const noexcept { return last_estimate_; }
    [[nodiscard]] const EstimatorDiagnostics& diagnostics() const noexcept { return diag_; }
    void reset() noexcept;

private:
    struct Point {
        Nanos t_ref_ns{0};
        Nanos offset_ns{0};
        Nanos rtt_ns{0};
    };

    void clear_window_for_new_epoch(std::uint32_t next_epoch) noexcept;
    void refit_model(
        const SyncExchangeSample& sample,
        Nanos raw_offset_ns,
        Nanos raw_rtt_ns,
        bool rejected_outlier,
        bool step_detected,
        bool restart_detected,
        ClockEstimate& out_estimate
    );

    EstimatorConfig cfg_{};
    BoundedRingBuffer<Point, kMaxWindow> window_{};
    BoundedRingBuffer<Point, 8> pending_step_points_{};

    ClockDomain locked_ref_domain_{ClockDomain::Unknown};
    ClockDomain locked_local_domain_{ClockDomain::Unknown};
    std::string locked_ref_clock_id_{};
    std::string locked_local_clock_id_{};

    bool has_last_sample_{false};
    std::uint32_t last_sample_epoch_{0};
    Nanos last_local_t2_ns_{0};
    Nanos last_ref_t1_ns_{0};
    std::uint32_t estimator_epoch_{0};

    Nanos min_rtt_ns_{0};
    bool has_min_rtt_{false};

    ClockEstimate last_estimate_{};
    EstimatorDiagnostics diag_{};
};

}  // namespace ptsp
