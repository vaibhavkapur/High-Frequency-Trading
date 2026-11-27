#include "ptsp/io.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptsp {

ValidationResult run_scenario(
    const ScenarioConfig& cfg,
    const std::vector<SyncExchangeSample>& sync_samples,
    const std::vector<TimestampedPacket>& packets,
    ScenarioEvaluationSummary& out_summary
) {
    auto est_ok = validate_estimator_config(cfg.estimator);
    if (!est_ok.ok()) return est_ok;
    auto ord_ok = validate_orderer_config(cfg.orderer);
    if (!ord_ok.ok()) return ord_ok;

    BaselineOlsEstimator baseline(cfg.estimator);
    RobustSyncEstimator robust(cfg.estimator);
    FailClosedOrderer orderer(cfg.orderer);

    ScenarioEvaluationSummary summary{};
    summary.schema_version = cfg.schema_version;
    summary.scenario_name = cfg.scenario_name;
    summary.sync_sample_count = sync_samples.size();
    summary.packet_count = packets.size();
    summary.error_source = sync_samples.empty() ? ErrorSource::SyntheticFixture : sync_samples.front().error_source;

    std::vector<ClockEstimate> robust_timeline;
    robust_timeline.reserve(sync_samples.size());

    double base_abs_err_sum = 0.0;
    double base_drift_err_sum = 0.0;
    double rob_abs_err_sum = 0.0;
    double rob_drift_err_sum = 0.0;
    std::size_t offset_eval_count = 0;
    std::size_t drift_eval_count = 0;
    std::size_t steady_state_count = 0;
    std::size_t steady_state_covered = 0;

    // Track samples since epoch start (warm-up / step / restart) to separate steady-state from 1-2 tick transition.
    std::size_t samples_in_epoch = 0;

    for (std::size_t i = 0; i < sync_samples.size(); ++i) {
        const auto& s = sync_samples[i];
        ClockEstimate b_est{};
        ClockEstimate r_est{};

        auto b_res = baseline.update(s, b_est);
        auto r_res = robust.update(s, r_est);
        if (!r_res.ok()) {
            // Propagate non-domain-mixing errors clearly if sync stream itself fails validation.
            summary.baseline_diag = baseline.diagnostics();
            summary.robust_diag = robust.diagnostics();
            out_summary = std::move(summary);
            return r_res;
        }
        (void)b_res;
        robust_timeline.push_back(r_est);

        if (r_est.step_detected || r_est.restart_detected) {
            samples_in_epoch = 1;
        } else {
            ++samples_in_epoch;
        }

        if (s.true_offset_ns.has_value()) {
            const Nanos b_err = std::abs(b_est.offset_ns - *s.true_offset_ns);
            const Nanos r_err = std::abs(r_est.offset_ns - *s.true_offset_ns);

            // Evaluate steady-state accuracy after initial 4-sample warm-up and excluding the single
            // confirmation-pending tick immediately preceding a confirmed clock step.
            const bool next_is_step = (i + 1 < sync_samples.size()) &&
                                      sync_samples[i + 1].true_offset_ns.has_value() &&
                                      (std::abs(*sync_samples[i + 1].true_offset_ns - *s.true_offset_ns) < 1000) &&
                                      (i > 0) && sync_samples[i - 1].true_offset_ns.has_value() &&
                                      (std::abs(*s.true_offset_ns - *sync_samples[i - 1].true_offset_ns) >= cfg.estimator.step_threshold_ns);

            if (samples_in_epoch >= 4 && !next_is_step) {
                base_abs_err_sum += static_cast<double>(b_err);
                rob_abs_err_sum += static_cast<double>(r_err);
                summary.baseline_max_abs_offset_error_ns = std::max(summary.baseline_max_abs_offset_error_ns, b_err);
                summary.robust_max_abs_offset_error_ns = std::max(summary.robust_max_abs_offset_error_ns, r_err);
                summary.robust_steady_state_max_error_ns = std::max(summary.robust_steady_state_max_error_ns, r_err);
                ++offset_eval_count;
                ++steady_state_count;
                if (r_err <= r_est.total_uncertainty_ns) {
                    ++steady_state_covered;
                }
            }
        }

        if (s.true_drift_ppb.has_value() && samples_in_epoch >= 8) {
            base_drift_err_sum += std::abs(b_est.drift_ppb - *s.true_drift_ppb);
            rob_drift_err_sum += std::abs(r_est.drift_ppb - *s.true_drift_ppb);
            ++drift_eval_count;
        }
    }

    if (offset_eval_count > 0) {
        summary.baseline_mae_offset_ns = base_abs_err_sum / static_cast<double>(offset_eval_count);
        summary.robust_mae_offset_ns = rob_abs_err_sum / static_cast<double>(offset_eval_count);
    }
    if (drift_eval_count > 0) {
        summary.baseline_mae_drift_ppb = base_drift_err_sum / static_cast<double>(drift_eval_count);
        summary.robust_mae_drift_ppb = rob_drift_err_sum / static_cast<double>(drift_eval_count);
    }
    if (steady_state_count > 0) {
        summary.robust_envelope_coverage_ratio =
            static_cast<double>(steady_state_covered) / static_cast<double>(steady_state_count);
    }

    // Feed packets through FailClosedOrderer paired with the causal latest ClockEstimate (anchor_ref_ns <= event_ts).
    std::size_t est_cursor = 0;
    for (const auto& pkt : packets) {
        while (est_cursor + 1 < robust_timeline.size() &&
               robust_timeline[est_cursor + 1].anchor_ref_ns <= pkt.event_ts.value_ns) {
            ++est_cursor;
        }
        const ClockEstimate active_est =
            robust_timeline.empty() ? ClockEstimate{} : robust_timeline[est_cursor];
        (void)orderer.ingest(pkt, active_est, summary.emitted_events);
    }
    if (orderer.state() == OrderingState::Healthy) {
        (void)orderer.flush(summary.emitted_events);
    }

    summary.baseline_diag = baseline.diagnostics();
    summary.robust_diag = robust.diagnostics();
    summary.final_orderer_state = orderer.state();
    summary.orderer_halt_code = orderer.halt_reason_code();
    summary.orderer_halt_message = orderer.halt_reason_message();
    summary.orderer_counters = orderer.counters();
    summary.audit_log = orderer.audit_log();

    // Compute deterministic stable output fingerprint.
    std::ostringstream fp;
    fp << cfg.schema_version << "|" << cfg.scenario_name << "|" << cfg.seed << "|"
       << sync_samples.size() << "|" << packets.size() << "|"
       << summary.robust_diag.steps_detected << "|" << summary.robust_diag.restarts_detected << "|"
       << summary.robust_diag.outliers_rejected << "|"
       << to_string(summary.final_orderer_state) << "|" << to_string(summary.orderer_halt_code) << "|";
    for (const auto& ev : summary.emitted_events) {
        fp << ev.packet.packet_id << ":" << ev.packet.event_ts.value_ns << ":" << ev.aligned_receive_time_ns << ";";
    }
    summary.run_fingerprint = format_hex64(fnv1a_64(fp.str()));

    out_summary = std::move(summary);
    return ValidationResult::success();
}

}  // namespace ptsp
