#pragma once

#include "ptsp/adapters.hpp"
#include "ptsp/estimator.hpp"
#include "ptsp/fail_closed_orderer.hpp"
#include "ptsp/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ptsp {

struct ScenarioConfig {
    std::string schema_version{std::string(kSchemaVersion)};
    std::string scenario_name{"reference"};
    std::uint64_t seed{42};
    EstimatorConfig estimator{};
    OrdererConfig orderer{};
    std::string sync_fixture_path{};
    std::string packet_fixture_path{};
    Nanos max_allowed_offset_error_ns{250};
    double max_allowed_drift_error_ppb{50.0};
};

struct ScenarioEvaluationSummary {
    std::string schema_version{std::string(kSchemaVersion)};
    std::string scenario_name{};
    std::string run_fingerprint{};   // Stable deterministic output identifier
    ErrorSource error_source{ErrorSource::SyntheticFixture};

    std::size_t sync_sample_count{0};
    std::size_t packet_count{0};

    // Baseline vs Robust offset/drift recovery metrics (computed over ground-truth fixtures, excluding transient warm-up).
    double baseline_mae_offset_ns{0.0};
    Nanos baseline_max_abs_offset_error_ns{0};
    double baseline_mae_drift_ppb{0.0};

    double robust_mae_offset_ns{0.0};
    Nanos robust_max_abs_offset_error_ns{0};
    Nanos robust_steady_state_max_error_ns{0};
    double robust_mae_drift_ppb{0.0};
    double robust_envelope_coverage_ratio{0.0}; // Fraction of steady-state points where |true - est| <= total_uncertainty_ns

    EstimatorDiagnostics baseline_diag{};
    EstimatorDiagnostics robust_diag{};

    OrderingState final_orderer_state{OrderingState::Healthy};
    ErrorCode orderer_halt_code{ErrorCode::Ok};
    std::string orderer_halt_message{};
    OrdererCounters orderer_counters{};
    std::vector<OrderedEvent> emitted_events{};
    std::vector<OrdererDiagnosticEvent> audit_log{};
};

// Loaders fail closed on missing files, malformed rows, or schema version mismatch.
[[nodiscard]] ValidationResult load_scenario_config_kv(const std::string& path, ScenarioConfig& out_cfg);
[[nodiscard]] ValidationResult load_sync_samples_csv(const std::string& path, std::vector<SyncExchangeSample>& out_samples);
[[nodiscard]] ValidationResult load_packets_csv(const std::string& path, std::vector<TimestampedPacket>& out_packets);

// Deterministic scenario execution engine (pure offline replay).
[[nodiscard]] ValidationResult run_scenario(
    const ScenarioConfig& cfg,
    const std::vector<SyncExchangeSample>& sync_samples,
    const std::vector<TimestampedPacket>& packets,
    ScenarioEvaluationSummary& out_summary
);

// JSON serializers (zero external JSON library dependency, strictly escaped valid JSON).
[[nodiscard]] std::string json_escape(std::string_view s);
[[nodiscard]] std::string serialize_capability_report_json(const AdapterCapabilityReport& report);
[[nodiscard]] std::string serialize_scenario_summary_json(const ScenarioEvaluationSummary& summary);
[[nodiscard]] ValidationResult write_text_file(const std::string& path, const std::string& contents);

}  // namespace ptsp
