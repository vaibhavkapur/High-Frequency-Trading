#include "ptsp/estimator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>

namespace ptsp {

namespace {

constexpr double kMadToSigma = 1.4826;

double compute_median(double* data, std::size_t n) {
    std::sort(data, data + n);
    if (n % 2 == 1) {
        return data[n / 2];
    }
    return 0.5 * (data[n / 2 - 1] + data[n / 2]);
}

void compute_two_way(const SyncExchangeSample& s, Nanos& raw_offset_ns, Nanos& raw_rtt_ns) noexcept {
    const Nanos m2s = s.t2_ns - s.t1_ns;
    const Nanos s2m = s.t4_ns - s.t3_ns;
    // Use double before llround to avoid overflow on large offsets while preserving exact nanosecond precision
    // within +/- 2^52 ns (~52 days). For larger offsets, split safely via int64 division.
    const Nanos diff_half = (m2s / 2) - (s2m / 2);
    const Nanos rem = (m2s % 2) - (s2m % 2);
    raw_offset_ns = diff_half + (rem / 2);
    raw_rtt_ns = m2s + s2m;
}

struct LinearFit {
    double intercept_ns{0.0};
    double slope_ns_per_s{0.0};
    double residual_std_ns{0.0};
    double slope_std_ppb{0.0};
};

// Fit y_i = a + b * x_i where x_i is in seconds relative to anchor_ref_ns and y_i is offset in ns.
// Weights are positive (all 1.0 for unweighted OLS).
LinearFit weighted_linear_fit(
    const double* xs_sec,
    const double* ys_ns,
    const double* weights,
    std::size_t n
) {
    LinearFit fit{};
    if (n == 0) {
        return fit;
    }
    if (n == 1) {
        fit.intercept_ns = ys_ns[0];
        fit.slope_ns_per_s = 0.0;
        fit.residual_std_ns = 0.0;
        fit.slope_std_ppb = 0.0;
        return fit;
    }

    double sw = 0.0;
    double swx = 0.0;
    double swy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sw += weights[i];
        swx += weights[i] * xs_sec[i];
        swy += weights[i] * ys_ns[i];
    }
    if (sw <= 0.0) {
        fit.intercept_ns = ys_ns[n - 1];
        return fit;
    }

    const double mean_x = swx / sw;
    const double mean_y = swy / sw;

    double sxx = 0.0;
    double sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = xs_sec[i] - mean_x;
        const double dy = ys_ns[i] - mean_y;
        sxx += weights[i] * dx * dx;
        sxy += weights[i] * dx * dy;
    }

    if (sxx > 1e-12) {
        fit.slope_ns_per_s = sxy / sxx;
    } else {
        fit.slope_ns_per_s = 0.0;
    }
    fit.intercept_ns = mean_y - fit.slope_ns_per_s * mean_x;

    double sse = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double pred = fit.intercept_ns + fit.slope_ns_per_s * xs_sec[i];
        const double r = ys_ns[i] - pred;
        sse += weights[i] * r * r;
    }
    const double dof = (n > 2) ? static_cast<double>(n - 2) : 1.0;
    const double var = sse / dof;
    fit.residual_std_ns = std::sqrt(std::max(0.0, var));
    if (sxx > 1e-12) {
        fit.slope_std_ppb = fit.residual_std_ns / std::sqrt(sxx);
    } else {
        fit.slope_std_ppb = 0.0;
    }
    return fit;
}

}  // namespace

ValidationResult validate_estimator_config(const EstimatorConfig& cfg) {
    if (cfg.window_size < 2 || cfg.window_size > RobustSyncEstimator::kMaxWindow) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig window_size must be in [2, 128]"
        );
    }
    if (cfg.outlier_mad_multiplier <= 0.0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig outlier_mad_multiplier must be > 0"
        );
    }
    if (cfg.outlier_min_mad_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig outlier_min_mad_ns must be >= 0"
        );
    }
    if (cfg.rtt_spike_factor < 1.0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig rtt_spike_factor must be >= 1.0"
        );
    }
    if (cfg.rtt_slack_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig rtt_slack_ns must be >= 0"
        );
    }
    if (cfg.step_threshold_ns <= 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig step_threshold_ns must be > 0"
        );
    }
    if (cfg.step_confirm_count == 0 || cfg.step_confirm_count > 8) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig step_confirm_count must be in [1, 8]"
        );
    }
    if (cfg.base_measurement_noise_ns < 0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig base_measurement_noise_ns must be >= 0"
        );
    }
    if (cfg.huber_delta_ns <= 0.0) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig huber_delta_ns must be > 0"
        );
    }
    if (cfg.irls_iterations == 0 || cfg.irls_iterations > 20) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "EstimatorConfig irls_iterations must be in [1, 20]"
        );
    }
    return ValidationResult::success();
}

ValidationResult align_local_to_reference(
    const ProvenancedTimestamp& local_ts,
    const ClockEstimate& estimate,
    ProvenancedTimestamp& out_aligned
) {
    auto ts_valid = validate_timestamp(local_ts);
    if (!ts_valid.ok()) {
        return ts_valid;
    }
    if (!estimate.valid) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "Cannot align timestamp using an uninitialized/invalid ClockEstimate"
        );
    }
    if (local_ts.provenance.domain != estimate.local_domain) {
        std::ostringstream oss;
        oss << "Cannot align timestamp from domain " << to_string(local_ts.provenance.domain)
            << " using estimator locked to local domain " << to_string(estimate.local_domain);
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }

    const Nanos approx_ref_ns = local_ts.value_ns - estimate.offset_ns;
    const double dt_sec = static_cast<double>(approx_ref_ns - estimate.anchor_ref_ns) * 1e-9;
    const double predicted_offset = static_cast<double>(estimate.offset_ns) + estimate.drift_ppb * dt_sec;
    const Nanos aligned_ns = local_ts.value_ns - static_cast<Nanos>(std::llround(predicted_offset));

    const double extrap_sec = std::abs(static_cast<double>(aligned_ns - estimate.anchor_ref_ns) * 1e-9);
    const Nanos drift_extrap_unc = static_cast<Nanos>(std::llround(estimate.drift_uncertainty_ppb * extrap_sec));

    out_aligned.value_ns = aligned_ns;
    out_aligned.uncertainty_ns = local_ts.uncertainty_ns + estimate.total_uncertainty_ns + drift_extrap_unc;
    out_aligned.provenance.domain = estimate.reference_domain;
    out_aligned.provenance.role = local_ts.provenance.role;
    out_aligned.provenance.capture_method = local_ts.provenance.capture_method;
    out_aligned.provenance.error_source = ErrorSource::EstimatedModel;
    out_aligned.provenance.clock_id = estimate.reference_clock_id + "<-" + local_ts.provenance.clock_id;
    out_aligned.provenance.epoch_id = estimate.estimator_epoch;
    return ValidationResult::success();
}

// ---------------- BaselineOlsEstimator ----------------

BaselineOlsEstimator::BaselineOlsEstimator(EstimatorConfig cfg) : cfg_(cfg) {}

void BaselineOlsEstimator::reset() noexcept {
    window_.clear();
    locked_ref_domain_ = ClockDomain::Unknown;
    locked_local_domain_ = ClockDomain::Unknown;
    min_rtt_ns_ = 0;
    has_min_rtt_ = false;
    last_estimate_ = {};
    diag_ = {};
}

ValidationResult BaselineOlsEstimator::update(const SyncExchangeSample& sample, ClockEstimate& out_estimate) {
    ++diag_.samples_processed;
    auto valid = validate_sync_sample(sample);
    if (!valid.ok()) {
        ++diag_.validation_errors;
        return valid;
    }

    if (locked_ref_domain_ == ClockDomain::Unknown) {
        locked_ref_domain_ = sample.reference_domain;
        locked_local_domain_ = sample.local_domain;
    } else if (sample.reference_domain != locked_ref_domain_ || sample.local_domain != locked_local_domain_) {
        ++diag_.domain_errors;
        return ValidationResult::fail(
            ErrorCode::ClockDomainMismatch,
            "BaselineOlsEstimator refuses to mix clock domains"
        );
    }

    Nanos raw_offset_ns = 0;
    Nanos raw_rtt_ns = 0;
    compute_two_way(sample, raw_offset_ns, raw_rtt_ns);

    if (!has_min_rtt_ || raw_rtt_ns < min_rtt_ns_) {
        min_rtt_ns_ = raw_rtt_ns;
        has_min_rtt_ = true;
    }

    while (window_.size() >= cfg_.window_size) {
        Point dropped{};
        (void)window_.try_pop_front(dropped);
    }
    window_.push_overwrite(Point{sample.t1_ns, raw_offset_ns, raw_rtt_ns});
    ++diag_.samples_accepted;

    const std::size_t n = window_.size();
    std::array<double, kMaxWindow> xs{};
    std::array<double, kMaxWindow> ys{};
    std::array<double, kMaxWindow> ws{};
    const Nanos anchor = sample.t1_ns;
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = static_cast<double>(window_[i].t_ref_ns - anchor) * 1e-9;
        ys[i] = static_cast<double>(window_[i].offset_ns);
        ws[i] = 1.0;
    }

    const LinearFit fit = weighted_linear_fit(xs.data(), ys.data(), ws.data(), n);
    const Nanos asym_bound = std::max<Nanos>(0, (raw_rtt_ns - min_rtt_ns_) / 2);
    const Nanos res_std = static_cast<Nanos>(std::llround(fit.residual_std_ns));

    ClockEstimate est{};
    est.valid = true;
    est.seq = sample.seq;
    est.estimator_epoch = sample.epoch_id;
    est.reference_domain = sample.reference_domain;
    est.local_domain = sample.local_domain;
    est.reference_clock_id = sample.reference_clock_id;
    est.local_clock_id = sample.local_clock_id;
    est.error_source = ErrorSource::EstimatedModel;
    est.input_error_source = sample.error_source;
    est.anchor_ref_ns = anchor;
    est.offset_ns = static_cast<Nanos>(std::llround(fit.intercept_ns));
    est.drift_ppb = fit.slope_ns_per_s;
    est.residual_std_ns = res_std;
    est.asymmetry_bound_ns = asym_bound;
    est.measurement_noise_ns = cfg_.base_measurement_noise_ns;
    est.total_uncertainty_ns = cfg_.base_measurement_noise_ns + asym_bound + (2 * res_std);
    est.drift_uncertainty_ppb = 2.0 * fit.slope_std_ppb;
    est.sample_rejected_outlier = false;
    est.step_detected = false;
    est.restart_detected = false;
    est.raw_offset_ns = raw_offset_ns;
    est.raw_rtt_ns = raw_rtt_ns;
    est.min_rtt_ns = min_rtt_ns_;

    last_estimate_ = est;
    out_estimate = est;
    return ValidationResult::success();
}

// ---------------- RobustSyncEstimator ----------------

RobustSyncEstimator::RobustSyncEstimator(EstimatorConfig cfg) : cfg_(cfg) {}

void RobustSyncEstimator::reset() noexcept {
    window_.clear();
    pending_step_points_.clear();
    locked_ref_domain_ = ClockDomain::Unknown;
    locked_local_domain_ = ClockDomain::Unknown;
    locked_ref_clock_id_.clear();
    locked_local_clock_id_.clear();
    has_last_sample_ = false;
    last_sample_epoch_ = 0;
    last_local_t2_ns_ = 0;
    last_ref_t1_ns_ = 0;
    estimator_epoch_ = 0;
    min_rtt_ns_ = 0;
    has_min_rtt_ = false;
    last_estimate_ = {};
    diag_ = {};
}

void RobustSyncEstimator::clear_window_for_new_epoch(std::uint32_t next_epoch) noexcept {
    window_.clear();
    pending_step_points_.clear();
    estimator_epoch_ = next_epoch;
}

ValidationResult RobustSyncEstimator::update(const SyncExchangeSample& sample, ClockEstimate& out_estimate) {
    ++diag_.samples_processed;
    auto valid = validate_sync_sample(sample);
    if (!valid.ok()) {
        ++diag_.validation_errors;
        return valid;
    }

    if (locked_ref_domain_ == ClockDomain::Unknown) {
        locked_ref_domain_ = sample.reference_domain;
        locked_local_domain_ = sample.local_domain;
        locked_ref_clock_id_ = sample.reference_clock_id;
        locked_local_clock_id_ = sample.local_clock_id;
    } else if (sample.reference_domain != locked_ref_domain_ || sample.local_domain != locked_local_domain_) {
        ++diag_.domain_errors;
        std::ostringstream oss;
        oss << "RobustSyncEstimator refuses to mix clock domains: expected ("
            << to_string(locked_ref_domain_) << ", " << to_string(locked_local_domain_)
            << ") but received (" << to_string(sample.reference_domain) << ", "
            << to_string(sample.local_domain) << ")";
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }

    Nanos raw_offset_ns = 0;
    Nanos raw_rtt_ns = 0;
    compute_two_way(sample, raw_offset_ns, raw_rtt_ns);

    // 1) Detect host/clock restart: epoch changed OR local monotonic clock jumped backwards while ref time advanced.
    bool restart_detected = false;
    if (has_last_sample_) {
        const bool epoch_changed = (sample.epoch_id != last_sample_epoch_);
        const bool local_counter_regressed =
            (sample.t1_ns >= last_ref_t1_ns_) && (sample.t2_ns < last_local_t2_ns_);
        if (epoch_changed || local_counter_regressed) {
            restart_detected = true;
            ++diag_.restarts_detected;
            clear_window_for_new_epoch(estimator_epoch_ + 1);
            min_rtt_ns_ = raw_rtt_ns;
            has_min_rtt_ = true;
        }
    }

    has_last_sample_ = true;
    last_sample_epoch_ = sample.epoch_id;
    last_local_t2_ns_ = sample.t2_ns;
    last_ref_t1_ns_ = sample.t1_ns;

    if (!has_min_rtt_) {
        min_rtt_ns_ = raw_rtt_ns;
        has_min_rtt_ = true;
    }

    const Point candidate{sample.t1_ns, raw_offset_ns, raw_rtt_ns};

    if (restart_detected || window_.size() < 4) {
        if (raw_rtt_ns < min_rtt_ns_) {
            min_rtt_ns_ = raw_rtt_ns;
        }
        window_.push_overwrite(candidate);
        ++diag_.samples_accepted;
        refit_model(sample, raw_offset_ns, raw_rtt_ns, false, false, restart_detected, out_estimate);
        return ValidationResult::success();
    }

    // 2) Compute prediction from current healthy model.
    const double dt_sec = static_cast<double>(sample.t1_ns - last_estimate_.anchor_ref_ns) * 1e-9;
    const double predicted_offset_ns =
        static_cast<double>(last_estimate_.offset_ns) + last_estimate_.drift_ppb * dt_sec;
    const double residual_ns = static_cast<double>(raw_offset_ns) - predicted_offset_ns;

    const double rtt_limit =
        static_cast<double>(min_rtt_ns_) * cfg_.rtt_spike_factor + static_cast<double>(cfg_.rtt_slack_ns);
    const bool rtt_is_spike = static_cast<double>(raw_rtt_ns) > rtt_limit;

    // 3) Check for true clock step (large offset residual WITHOUT an RTT spike).
    if (!rtt_is_spike && std::abs(residual_ns) >= static_cast<double>(cfg_.step_threshold_ns)) {
        bool consistent_with_pending = true;
        if (!pending_step_points_.empty()) {
            const Nanos prev_pending_offset = pending_step_points_.newest().offset_ns;
            if (std::abs(raw_offset_ns - prev_pending_offset) > (cfg_.step_threshold_ns / 2)) {
                consistent_with_pending = false;
            }
        }
        if (!consistent_with_pending) {
            pending_step_points_.clear();
        }
        pending_step_points_.push_overwrite(candidate);

        if (pending_step_points_.size() >= cfg_.step_confirm_count) {
            // Confirmed clock step: transition to a new epoch and seed window with confirmed step points.
            ++diag_.steps_detected;
            BoundedRingBuffer<Point, 8> confirmed = pending_step_points_;
            clear_window_for_new_epoch(estimator_epoch_ + 1);
            for (std::size_t i = 0; i < confirmed.size(); ++i) {
                if (confirmed[i].rtt_ns < min_rtt_ns_) {
                    min_rtt_ns_ = confirmed[i].rtt_ns;
                }
                window_.push_overwrite(confirmed[i]);
                ++diag_.samples_accepted;
            }
            refit_model(sample, raw_offset_ns, raw_rtt_ns, false, true, false, out_estimate);
            return ValidationResult::success();
        }

        // First suspect sample of a potential step: do not yet corrupt window; report outlier pending confirmation.
        ++diag_.outliers_rejected;
        refit_model(sample, raw_offset_ns, raw_rtt_ns, true, false, false, out_estimate);
        return ValidationResult::success();
    }

    // Not a step candidate -> clear pending step buffer.
    pending_step_points_.clear();

    // 4) Robust MAD outlier rejection + RTT spike filtering (handles asymmetric spikes & queuing jitter).
    const std::size_t n = window_.size();
    std::array<double, kMaxWindow> abs_res{};
    for (std::size_t i = 0; i < n; ++i) {
        const double wi_dt = static_cast<double>(window_[i].t_ref_ns - last_estimate_.anchor_ref_ns) * 1e-9;
        const double wi_pred = static_cast<double>(last_estimate_.offset_ns) + last_estimate_.drift_ppb * wi_dt;
        abs_res[i] = std::abs(static_cast<double>(window_[i].offset_ns) - wi_pred);
    }
    const double mad = compute_median(abs_res.data(), n);
    const double robust_sigma = std::max(kMadToSigma * mad, static_cast<double>(cfg_.outlier_min_mad_ns));
    const bool mad_outlier = std::abs(residual_ns) > (cfg_.outlier_mad_multiplier * robust_sigma);

    if (rtt_is_spike || mad_outlier) {
        ++diag_.outliers_rejected;
        refit_model(sample, raw_offset_ns, raw_rtt_ns, true, false, false, out_estimate);
        return ValidationResult::success();
    }

    // 5) Inlier sample: accept into bounded sliding window and refit via Huber IRLS.
    if (raw_rtt_ns < min_rtt_ns_) {
        min_rtt_ns_ = raw_rtt_ns;
    }
    while (window_.size() >= cfg_.window_size) {
        Point dropped{};
        (void)window_.try_pop_front(dropped);
    }
    window_.push_overwrite(candidate);
    ++diag_.samples_accepted;

    refit_model(sample, raw_offset_ns, raw_rtt_ns, false, false, false, out_estimate);
    return ValidationResult::success();
}

void RobustSyncEstimator::refit_model(
    const SyncExchangeSample& sample,
    Nanos raw_offset_ns,
    Nanos raw_rtt_ns,
    bool rejected_outlier,
    bool step_detected,
    bool restart_detected,
    ClockEstimate& out_estimate
) {
    const std::size_t n = window_.size();
    const Nanos anchor = sample.t1_ns;

    std::array<double, kMaxWindow> xs{};
    std::array<double, kMaxWindow> ys{};
    std::array<double, kMaxWindow> ws{};

    Nanos window_max_excess_rtt = 0;
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = static_cast<double>(window_[i].t_ref_ns - anchor) * 1e-9;
        ys[i] = static_cast<double>(window_[i].offset_ns);
        // Down-weight samples with larger excess RTT slightly, since minimum-RTT packets have least queuing asymmetry.
        const Nanos excess_rtt = std::max<Nanos>(0, window_[i].rtt_ns - min_rtt_ns_);
        if (excess_rtt > window_max_excess_rtt) {
            window_max_excess_rtt = excess_rtt;
        }
        ws[i] = 1.0 / (1.0 + static_cast<double>(excess_rtt) / 500.0);
    }

    LinearFit fit{};
    if (n == 1) {
        fit.intercept_ns = ys[0];
        fit.slope_ns_per_s = 0.0;
        fit.residual_std_ns = static_cast<double>(cfg_.base_measurement_noise_ns);
        fit.slope_std_ppb = 0.0;
    } else {
        // Bounded Huber IRLS for outlier-resistant slope and offset.
        fit = weighted_linear_fit(xs.data(), ys.data(), ws.data(), n);
        for (std::size_t iter = 0; iter < cfg_.irls_iterations; ++iter) {
            std::array<double, kMaxWindow> iter_ws{};
            for (std::size_t i = 0; i < n; ++i) {
                const double pred = fit.intercept_ns + fit.slope_ns_per_s * xs[i];
                const double r = std::abs(ys[i] - pred);
                const double huber_w = (r <= cfg_.huber_delta_ns) ? 1.0 : (cfg_.huber_delta_ns / r);
                iter_ws[i] = ws[i] * huber_w;
            }
            fit = weighted_linear_fit(xs.data(), ys.data(), iter_ws.data(), n);
        }
    }

    // Asymmetry uncertainty bound:
    // For inliers, use the conservative half excess RTT of the current sample (and recent window median/max).
    // For rejected asymmetric spikes, reflect the elevated path asymmetry in total_uncertainty_ns so
    // downstream fail-closed monitors are alerted if asymmetry persists or exceeds the skew budget!
    const Nanos current_excess_rtt = std::max<Nanos>(0, raw_rtt_ns - min_rtt_ns_);
    const Nanos asym_bound = rejected_outlier
        ? std::max(current_excess_rtt / 2, window_max_excess_rtt / 2)
        : std::max(current_excess_rtt / 2, window_max_excess_rtt / 4);

    const Nanos res_std = static_cast<Nanos>(std::llround(fit.residual_std_ns));
    const Nanos total_unc = cfg_.base_measurement_noise_ns + asym_bound + (3 * res_std);

    ClockEstimate est{};
    est.valid = true;
    est.seq = sample.seq;
    est.estimator_epoch = estimator_epoch_;
    est.reference_domain = sample.reference_domain;
    est.local_domain = sample.local_domain;
    est.reference_clock_id = sample.reference_clock_id;
    est.local_clock_id = sample.local_clock_id;
    est.error_source = ErrorSource::EstimatedModel;
    est.input_error_source = sample.error_source;
    est.anchor_ref_ns = anchor;
    est.offset_ns = static_cast<Nanos>(std::llround(fit.intercept_ns));
    est.drift_ppb = fit.slope_ns_per_s;
    est.residual_std_ns = res_std;
    est.asymmetry_bound_ns = asym_bound;
    est.measurement_noise_ns = cfg_.base_measurement_noise_ns;
    est.total_uncertainty_ns = total_unc;
    est.drift_uncertainty_ppb = (n >= 3) ? (3.0 * fit.slope_std_ppb) : 500.0;
    est.sample_rejected_outlier = rejected_outlier;
    est.step_detected = step_detected;
    est.restart_detected = restart_detected;
    est.raw_offset_ns = raw_offset_ns;
    est.raw_rtt_ns = raw_rtt_ns;
    est.min_rtt_ns = min_rtt_ns_;

    last_estimate_ = est;
    out_estimate = est;
}

}  // namespace ptsp
