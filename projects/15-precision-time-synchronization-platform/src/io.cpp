#include "ptsp/io.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ptsp {

namespace {

std::string trim(std::string_view sv) {
    std::size_t start = 0;
    while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
        ++start;
    }
    std::size_t end = sv.size();
    while (end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' || sv[end - 1] == '\n')) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

std::vector<std::string> split_csv_line(std::string_view line) {
    std::vector<std::string> cols;
    std::size_t pos = 0;
    while (pos <= line.size()) {
        const std::size_t comma = line.find(',', pos);
        if (comma == std::string_view::npos) {
            cols.push_back(trim(line.substr(pos)));
            break;
        }
        cols.push_back(trim(line.substr(pos, comma - pos)));
        pos = comma + 1;
    }
    return cols;
}

bool parse_i64(const std::string& s, std::int64_t& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_u32(const std::string& s, std::uint32_t& out) {
    std::uint64_t tmp = 0;
    if (!parse_u64(s, tmp) || tmp > 0xFFFFFFFFULL) return false;
    out = static_cast<std::uint32_t>(tmp);
    return true;
}

bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    try {
        std::size_t idx = 0;
        out = std::stod(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_bool(const std::string& s, bool& out) {
    if (s == "true" || s == "1") {
        out = true;
        return true;
    }
    if (s == "false" || s == "0") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

ValidationResult load_scenario_config_kv(const std::string& path, ScenarioConfig& out_cfg) {
    if (path.empty()) {
        return ValidationResult::fail(ErrorCode::MissingConfig, "Config path is empty");
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return ValidationResult::fail(ErrorCode::MissingConfig, "Cannot open config file: " + path);
    }

    ScenarioConfig cfg{};
    bool saw_schema = false;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) {
            return ValidationResult::fail(
                ErrorCode::ParseError,
                "Malformed config line " + std::to_string(line_no) + ": missing '='"
            );
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));

        if (key == "schema_version") {
            saw_schema = true;
            cfg.schema_version = val;
            if (val != kSchemaVersion) {
                return ValidationResult::fail(
                    ErrorCode::SchemaVersionMismatch,
                    "Unsupported config schema_version: " + val
                );
            }
        } else if (key == "scenario_name") {
            cfg.scenario_name = val;
        } else if (key == "seed") {
            if (!parse_u64(val, cfg.seed)) return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid seed");
        } else if (key == "sync_fixture_path") {
            cfg.sync_fixture_path = val;
        } else if (key == "packet_fixture_path") {
            cfg.packet_fixture_path = val;
        } else if (key == "window_size") {
            std::uint64_t v = 0;
            if (!parse_u64(val, v)) return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid window_size");
            cfg.estimator.window_size = static_cast<std::size_t>(v);
        } else if (key == "outlier_mad_multiplier") {
            if (!parse_double(val, cfg.estimator.outlier_mad_multiplier)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid outlier_mad_multiplier");
            }
        } else if (key == "rtt_spike_factor") {
            if (!parse_double(val, cfg.estimator.rtt_spike_factor)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid rtt_spike_factor");
            }
        } else if (key == "step_threshold_ns") {
            if (!parse_i64(val, cfg.estimator.step_threshold_ns)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid step_threshold_ns");
            }
        } else if (key == "step_confirm_count") {
            std::uint64_t v = 0;
            if (!parse_u64(val, v)) return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid step_confirm_count");
            cfg.estimator.step_confirm_count = static_cast<std::size_t>(v);
        } else if (key == "expected_event_domain") {
            auto d = parse_clock_domain(val);
            if (!d) return ValidationResult::fail(ErrorCode::UnsupportedClockDomain, "Unknown expected_event_domain: " + val);
            cfg.orderer.expected_event_domain = *d;
        } else if (key == "expected_receive_domain") {
            auto d = parse_clock_domain(val);
            if (!d) return ValidationResult::fail(ErrorCode::UnsupportedClockDomain, "Unknown expected_receive_domain: " + val);
            cfg.orderer.expected_receive_domain = *d;
        } else if (key == "max_buffered_events") {
            std::uint64_t v = 0;
            if (!parse_u64(val, v)) return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid max_buffered_events");
            cfg.orderer.max_buffered_events = static_cast<std::size_t>(v);
        } else if (key == "reorder_window_ns") {
            if (!parse_i64(val, cfg.orderer.reorder_window_ns)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid reorder_window_ns");
            }
        } else if (key == "max_skew_ns") {
            if (!parse_i64(val, cfg.orderer.max_skew_ns)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid max_skew_ns");
            }
        } else if (key == "max_causal_slack_ns") {
            if (!parse_i64(val, cfg.orderer.max_causal_slack_ns)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid max_causal_slack_ns");
            }
        } else if (key == "fail_closed_on_ambiguity") {
            if (!parse_bool(val, cfg.orderer.fail_closed_on_ambiguity)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid fail_closed_on_ambiguity");
            }
        } else if (key == "max_allowed_offset_error_ns") {
            if (!parse_i64(val, cfg.max_allowed_offset_error_ns)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid max_allowed_offset_error_ns");
            }
        } else if (key == "max_allowed_drift_error_ppb") {
            if (!parse_double(val, cfg.max_allowed_drift_error_ppb)) {
                return ValidationResult::fail(ErrorCode::InvalidConfig, "Invalid max_allowed_drift_error_ppb");
            }
        } else {
            return ValidationResult::fail(
                ErrorCode::InvalidConfig,
                "Unknown configuration key '" + key + "' at line " + std::to_string(line_no)
            );
        }
    }

    if (!saw_schema) {
        return ValidationResult::fail(
            ErrorCode::InvalidConfig,
            "Config file missing required schema_version=" + std::string(kSchemaVersion)
        );
    }

    auto est_res = validate_estimator_config(cfg.estimator);
    if (!est_res.ok()) return est_res;

    auto ord_res = validate_orderer_config(cfg.orderer);
    if (!ord_res.ok()) return ord_res;

    out_cfg = std::move(cfg);
    return ValidationResult::success();
}

ValidationResult load_sync_samples_csv(const std::string& path, std::vector<SyncExchangeSample>& out_samples) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return ValidationResult::fail(ErrorCode::IoError, "Cannot open sync fixture: " + path);
    }
    out_samples.clear();

    std::string line;
    std::size_t line_no = 0;
    bool saw_header = false;
    bool saw_schema = false;

    while (std::getline(ifs, line)) {
        ++line_no;
        const std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#') {
            if (t.find("schema_version=") != std::string::npos) {
                const auto pos = t.find("schema_version=");
                const std::string ver = trim(t.substr(pos + 15));
                if (ver != kSchemaVersion) {
                    return ValidationResult::fail(
                        ErrorCode::SchemaVersionMismatch,
                        "Sync fixture schema_version mismatch: " + ver
                    );
                }
                saw_schema = true;
            }
            continue;
        }
        if (!saw_header) {
            saw_header = true;
            continue;
        }

        const auto cols = split_csv_line(t);
        // Expected 15 columns:
        // seq,epoch_id,reference_domain,local_domain,reference_clock_id,local_clock_id,
        // capture_method,error_source,t1_ns,t2_ns,t3_ns,t4_ns,true_offset_ns,true_drift_ppb,true_asymmetry_ns
        if (cols.size() != 15) {
            return ValidationResult::fail(
                ErrorCode::ParseError,
                "Sync CSV line " + std::to_string(line_no) + " expected 15 columns, got " + std::to_string(cols.size())
            );
        }

        SyncExchangeSample s{};
        if (!parse_u64(cols[0], s.seq) || !parse_u32(cols[1], s.epoch_id)) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid seq/epoch_id at line " + std::to_string(line_no));
        }
        auto ref_d = parse_clock_domain(cols[2]);
        auto loc_d = parse_clock_domain(cols[3]);
        if (!ref_d || !loc_d) {
            return ValidationResult::fail(ErrorCode::UnsupportedClockDomain, "Invalid clock domain at line " + std::to_string(line_no));
        }
        s.reference_domain = *ref_d;
        s.local_domain = *loc_d;
        s.reference_clock_id = cols[4];
        s.local_clock_id = cols[5];

        auto cm = parse_capture_method(cols[6]);
        auto es = parse_error_source(cols[7]);
        if (!cm || !es) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid capture_method/error_source at line " + std::to_string(line_no));
        }
        s.capture_method = *cm;
        s.error_source = *es;

        if (!parse_i64(cols[8], s.t1_ns) || !parse_i64(cols[9], s.t2_ns) ||
            !parse_i64(cols[10], s.t3_ns) || !parse_i64(cols[11], s.t4_ns)) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid t1..t4 integer at line " + std::to_string(line_no));
        }
        if (!cols[12].empty()) {
            Nanos v = 0;
            if (!parse_i64(cols[12], v)) return ValidationResult::fail(ErrorCode::ParseError, "Invalid true_offset_ns");
            s.true_offset_ns = v;
        }
        if (!cols[13].empty()) {
            double v = 0.0;
            if (!parse_double(cols[13], v)) return ValidationResult::fail(ErrorCode::ParseError, "Invalid true_drift_ppb");
            s.true_drift_ppb = v;
        }
        if (!cols[14].empty()) {
            Nanos v = 0;
            if (!parse_i64(cols[14], v)) return ValidationResult::fail(ErrorCode::ParseError, "Invalid true_asymmetry_ns");
            s.true_asymmetry_ns = v;
        }

        out_samples.push_back(std::move(s));
    }

    if (!saw_schema) {
        return ValidationResult::fail(
            ErrorCode::SchemaVersionMismatch,
            "Sync fixture missing '# schema_version=" + std::string(kSchemaVersion) + "' header"
        );
    }
    return ValidationResult::success();
}

ValidationResult load_packets_csv(const std::string& path, std::vector<TimestampedPacket>& out_packets) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return ValidationResult::fail(ErrorCode::IoError, "Cannot open packet fixture: " + path);
    }
    out_packets.clear();

    std::string line;
    std::size_t line_no = 0;
    bool saw_header = false;
    bool saw_schema = false;

    while (std::getline(ifs, line)) {
        ++line_no;
        const std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#') {
            if (t.find("schema_version=") != std::string::npos) {
                const auto pos = t.find("schema_version=");
                const std::string ver = trim(t.substr(pos + 15));
                if (ver != kSchemaVersion) {
                    return ValidationResult::fail(
                        ErrorCode::SchemaVersionMismatch,
                        "Packet fixture schema_version mismatch: " + ver
                    );
                }
                saw_schema = true;
            }
            continue;
        }
        if (!saw_header) {
            saw_header = true;
            continue;
        }

        const auto cols = split_csv_line(t);
        // Expected 18 columns:
        // packet_id,sequence_id,source_id,symbol,
        // event_ns,event_unc_ns,event_domain,event_role,event_capture,event_error_source,event_clock_id,
        // rx_ns,rx_unc_ns,rx_domain,rx_role,rx_capture,rx_error_source,rx_clock_id
        if (cols.size() != 18) {
            return ValidationResult::fail(
                ErrorCode::ParseError,
                "Packet CSV line " + std::to_string(line_no) + " expected 18 columns, got " + std::to_string(cols.size())
            );
        }

        TimestampedPacket p{};
        if (!parse_u64(cols[0], p.packet_id) || !parse_u64(cols[1], p.sequence_id)) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid packet_id/sequence_id at line " + std::to_string(line_no));
        }
        p.source_id = cols[2];
        p.symbol = cols[3];

        auto ev_dom = parse_clock_domain(cols[6]);
        auto ev_role = parse_timestamp_role(cols[7]);
        auto ev_cap = parse_capture_method(cols[8]);
        auto ev_src = parse_error_source(cols[9]);
        if (!parse_i64(cols[4], p.event_ts.value_ns) || !parse_i64(cols[5], p.event_ts.uncertainty_ns) ||
            !ev_dom || !ev_role || !ev_cap || !ev_src) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid event_ts fields at line " + std::to_string(line_no));
        }
        p.event_ts.provenance.domain = *ev_dom;
        p.event_ts.provenance.role = *ev_role;
        p.event_ts.provenance.capture_method = *ev_cap;
        p.event_ts.provenance.error_source = *ev_src;
        p.event_ts.provenance.clock_id = cols[10];

        auto rx_dom = parse_clock_domain(cols[13]);
        auto rx_role = parse_timestamp_role(cols[14]);
        auto rx_cap = parse_capture_method(cols[15]);
        auto rx_src = parse_error_source(cols[16]);
        if (!parse_i64(cols[11], p.receive_ts.value_ns) || !parse_i64(cols[12], p.receive_ts.uncertainty_ns) ||
            !rx_dom || !rx_role || !rx_cap || !rx_src) {
            return ValidationResult::fail(ErrorCode::ParseError, "Invalid receive_ts fields at line " + std::to_string(line_no));
        }
        p.receive_ts.provenance.domain = *rx_dom;
        p.receive_ts.provenance.role = *rx_role;
        p.receive_ts.provenance.capture_method = *rx_cap;
        p.receive_ts.provenance.error_source = *rx_src;
        p.receive_ts.provenance.clock_id = cols[17];

        out_packets.push_back(std::move(p));
    }

    if (!saw_schema) {
        return ValidationResult::fail(
            ErrorCode::SchemaVersionMismatch,
            "Packet fixture missing '# schema_version=" + std::string(kSchemaVersion) + "' header"
        );
    }
    return ValidationResult::success();
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string serialize_capability_report_json(const AdapterCapabilityReport& r) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{\n"
        << "  \"schema_version\": \"" << kSchemaVersion << "\",\n"
        << "  \"adapter_name\": \"" << json_escape(r.adapter_name) << "\",\n"
        << "  \"platform\": \"" << json_escape(r.platform) << "\",\n"
        << "  \"capture_method\": \"" << to_string(r.capture_method) << "\",\n"
        << "  \"error_source\": \"" << to_string(r.error_source) << "\",\n"
        << "  \"available\": " << (r.available ? "true" : "false") << ",\n"
        << "  \"hardware_timestamping_supported\": " << (r.hardware_timestamping_supported ? "true" : "false") << ",\n"
        << "  \"status_code\": \"" << to_string(r.status_code) << "\",\n"
        << "  \"status_message\": \"" << json_escape(r.status_message) << "\",\n"
        << "  \"clock_resolution_ns\": " << r.clock_resolution_ns << ",\n"
        << "  \"median_read_latency_ns\": " << r.median_read_latency_ns << ",\n"
        << "  \"p99_read_latency_ns\": " << r.p99_read_latency_ns << ",\n"
        << "  \"measured_realtime_vs_monotonic_jitter_ns\": " << r.measured_realtime_vs_monotonic_jitter_ns << ",\n"
        << "  \"probe_samples\": " << r.probe_samples << "\n"
        << "}\n";
    return oss.str();
}

std::string serialize_scenario_summary_json(const ScenarioEvaluationSummary& s) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "{\n"
        << "  \"schema_version\": \"" << json_escape(s.schema_version) << "\",\n"
        << "  \"scenario_name\": \"" << json_escape(s.scenario_name) << "\",\n"
        << "  \"run_fingerprint\": \"" << json_escape(s.run_fingerprint) << "\",\n"
        << "  \"error_source\": \"" << to_string(s.error_source) << "\",\n"
        << "  \"sync_sample_count\": " << s.sync_sample_count << ",\n"
        << "  \"packet_count\": " << s.packet_count << ",\n"
        << "  \"baseline_estimator\": {\n"
        << "    \"mae_offset_ns\": " << s.baseline_mae_offset_ns << ",\n"
        << "    \"max_abs_offset_error_ns\": " << s.baseline_max_abs_offset_error_ns << ",\n"
        << "    \"mae_drift_ppb\": " << s.baseline_mae_drift_ppb << ",\n"
        << "    \"samples_processed\": " << s.baseline_diag.samples_processed << ",\n"
        << "    \"samples_accepted\": " << s.baseline_diag.samples_accepted << ",\n"
        << "    \"domain_errors\": " << s.baseline_diag.domain_errors << "\n"
        << "  },\n"
        << "  \"robust_estimator\": {\n"
        << "    \"mae_offset_ns\": " << s.robust_mae_offset_ns << ",\n"
        << "    \"max_abs_offset_error_ns\": " << s.robust_max_abs_offset_error_ns << ",\n"
        << "    \"steady_state_max_error_ns\": " << s.robust_steady_state_max_error_ns << ",\n"
        << "    \"mae_drift_ppb\": " << s.robust_mae_drift_ppb << ",\n"
        << "    \"envelope_coverage_ratio\": " << s.robust_envelope_coverage_ratio << ",\n"
        << "    \"samples_processed\": " << s.robust_diag.samples_processed << ",\n"
        << "    \"samples_accepted\": " << s.robust_diag.samples_accepted << ",\n"
        << "    \"outliers_rejected\": " << s.robust_diag.outliers_rejected << ",\n"
        << "    \"steps_detected\": " << s.robust_diag.steps_detected << ",\n"
        << "    \"restarts_detected\": " << s.robust_diag.restarts_detected << ",\n"
        << "    \"domain_errors\": " << s.robust_diag.domain_errors << "\n"
        << "  },\n"
        << "  \"orderer\": {\n"
        << "    \"final_state\": \"" << to_string(s.final_orderer_state) << "\",\n"
        << "    \"halt_code\": \"" << to_string(s.orderer_halt_code) << "\",\n"
        << "    \"halt_message\": \"" << json_escape(s.orderer_halt_message) << "\",\n"
        << "    \"packets_ingested\": " << s.orderer_counters.packets_ingested << ",\n"
        << "    \"packets_emitted\": " << s.orderer_counters.packets_emitted << ",\n"
        << "    \"packets_reordered\": " << s.orderer_counters.packets_reordered << ",\n"
        << "    \"domain_mismatches\": " << s.orderer_counters.domain_mismatches << ",\n"
        << "    \"invalid_packets\": " << s.orderer_counters.invalid_packets << ",\n"
        << "    \"skew_violations\": " << s.orderer_counters.skew_violations << ",\n"
        << "    \"causal_violations\": " << s.orderer_counters.causal_violations << ",\n"
        << "    \"ambiguity_halts\": " << s.orderer_counters.ambiguity_halts << ",\n"
        << "    \"buffer_overflows\": " << s.orderer_counters.buffer_overflows << ",\n"
        << "    \"rejected_while_halted\": " << s.orderer_counters.rejected_while_halted << "\n"
        << "  }\n"
        << "}\n";
    return oss.str();
}

ValidationResult write_text_file(const std::string& path, const std::string& contents) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            return ValidationResult::fail(ErrorCode::IoError, "Failed to create parent directory: " + ec.message());
        }
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return ValidationResult::fail(ErrorCode::IoError, "Cannot open output file for write: " + path);
    }
    ofs << contents;
    if (!ofs.good()) {
        return ValidationResult::fail(ErrorCode::IoError, "Failed writing to file: " + path);
    }
    return ValidationResult::success();
}

}  // namespace ptsp
