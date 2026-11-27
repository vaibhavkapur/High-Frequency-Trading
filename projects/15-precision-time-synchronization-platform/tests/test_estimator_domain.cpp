#include "ptsp/estimator.hpp"
#include "ptsp/io.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " — " << (msg) << "\n";      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    const std::filesystem::path fix_dir = std::filesystem::path(PTSP_SOURCE_DIR) / "fixtures";

    // 1) Reference linear drift recovery within documented error envelope (<= 100 ns offset, <= 15 ppb drift).
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "reference_drift.cfg").string(), cfg).ok(),
              "load reference_drift.cfg");
        std::vector<ptsp::SyncExchangeSample> samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), samples).ok(),
              "load reference_drift_sync.csv");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load reference_drift_packets.csv");

        ptsp::ScenarioEvaluationSummary sum{};
        CHECK(ptsp::run_scenario(cfg, samples, packets, sum).ok(), "run reference_drift");
        CHECK(sum.robust_steady_state_max_error_ns <= cfg.max_allowed_offset_error_ns,
              "Robust offset error must stay within documented error envelope (<= 100 ns)");
        CHECK(sum.robust_mae_drift_ppb <= cfg.max_allowed_drift_error_ppb,
              "Robust drift MAE must stay within documented error envelope (<= 15 ppb)");
        CHECK(sum.robust_envelope_coverage_ratio >= 0.95,
              "Uncertainty envelope must cover >= 95% of steady-state true offsets");
        CHECK(sum.error_source == ptsp::ErrorSource::SyntheticFixture,
              "Synthetic fixture error source must be preserved");
    }

    // 2) Clock step + single-packet outlier injection:
    //    Robust estimator detects step once, rejects outliers, and outperforms Baseline OLS by > 5x on MAE.
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "clock_step_and_outliers.cfg").string(), cfg).ok(),
              "load clock_step_and_outliers.cfg");
        std::vector<ptsp::SyncExchangeSample> samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), samples).ok(),
              "load step sync");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load step pkts");

        ptsp::ScenarioEvaluationSummary sum{};
        CHECK(ptsp::run_scenario(cfg, samples, packets, sum).ok(), "run clock_step_and_outliers");
        CHECK(sum.robust_diag.steps_detected == 1, "Must detect exactly 1 injected clock step");
        CHECK(sum.robust_diag.outliers_rejected >= 4, "Must reject all 4 injected delay spikes");
        CHECK(sum.robust_steady_state_max_error_ns <= cfg.max_allowed_offset_error_ns,
              "Robust estimator must recover post-step offset within <= 150 ns");
        CHECK(sum.robust_mae_offset_ns * 5.0 < sum.baseline_mae_offset_ns,
              "Robust estimator must significantly outperform unweighted baseline OLS under steps + outliers");
    }

    // 3) Packet asymmetry bursts: robust estimator filters asymmetric delay spikes and beats baseline OLS.
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "packet_asymmetry.cfg").string(), cfg).ok(),
              "load packet_asymmetry.cfg");
        std::vector<ptsp::SyncExchangeSample> samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), samples).ok(),
              "load asym sync");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load asym pkts");

        ptsp::ScenarioEvaluationSummary sum{};
        CHECK(ptsp::run_scenario(cfg, samples, packets, sum).ok(), "run packet_asymmetry");
        CHECK(sum.robust_diag.outliers_rejected >= 6, "Must reject periodic asymmetric delay bursts");
        CHECK(sum.robust_steady_state_max_error_ns <= cfg.max_allowed_offset_error_ns,
              "Robust offset error must stay within <= 150 ns despite asymmetric bursts");
        CHECK(sum.robust_mae_offset_ns * 4.0 < sum.baseline_mae_offset_ns,
              "Robust estimator must outperform baseline OLS under packet asymmetry");
    }

    // 4) Clock restart (epoch increment + -500 ms local counter reset): cleanly resets window and tracks new epoch.
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "clock_restart.cfg").string(), cfg).ok(),
              "load clock_restart.cfg");
        std::vector<ptsp::SyncExchangeSample> samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), samples).ok(),
              "load restart sync");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load restart pkts");

        ptsp::ScenarioEvaluationSummary sum{};
        CHECK(ptsp::run_scenario(cfg, samples, packets, sum).ok(), "run clock_restart");
        CHECK(sum.robust_diag.restarts_detected == 1, "Must detect exactly 1 clock restart");
        CHECK(sum.robust_steady_state_max_error_ns <= cfg.max_allowed_offset_error_ns,
              "Robust estimator must recover post-restart offset within <= 150 ns");
        CHECK(sum.robust_mae_offset_ns * 10.0 < sum.baseline_mae_offset_ns,
              "Baseline OLS mixes pre/post restart epochs while RobustSyncEstimator resets cleanly");
    }

    // 5) Estimator MUST refuse silently mixing clock domains.
    {
        ptsp::RobustSyncEstimator est{};
        ptsp::SyncExchangeSample s1{};
        s1.seq = 1;
        s1.reference_domain = ptsp::ClockDomain::Tai;
        s1.local_domain = ptsp::ClockDomain::MonotonicRaw;
        s1.t1_ns = 1'700'000'000'000'000'000LL;
        s1.t2_ns = s1.t1_ns + 1'500;
        s1.t3_ns = s1.t2_ns + 1'000;
        s1.t4_ns = s1.t3_ns + 500;

        ptsp::ClockEstimate out{};
        CHECK(est.update(s1, out).ok(), "First valid sample accepted");

        ptsp::SyncExchangeSample s2 = s1;
        s2.seq = 2;
        s2.reference_domain = ptsp::ClockDomain::Utc;  // Different domain!
        auto r = est.update(s2, out);
        CHECK(!r.ok() && r.code == ptsp::ErrorCode::ClockDomainMismatch,
              "Estimator must reject cross-domain mixing with ClockDomainMismatch");
        CHECK(est.diagnostics().domain_errors == 1, "domain_errors counter incremented");
    }

    std::cout << "[PASS] test_estimator_domain\n";
    return 0;
}
