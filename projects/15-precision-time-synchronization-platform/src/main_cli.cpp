#include "ptsp/adapters.hpp"
#include "ptsp/io.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cerr
        << "Precision Time Synchronization Platform CLI (ptsp_cli)\n"
        << "Usage:\n"
        << "  ptsp_cli probe [--interface <ifname>] [--ptp-device <path>] [--out <json>]\n"
        << "  ptsp_cli replay --config <config.kv> [--out <json>] [--audit-out <jsonl>]\n"
        << "  ptsp_cli demo --fixtures-dir <dir> --reports-dir <dir>\n";
}

std::string serialize_audit_jsonl(const std::vector<ptsp::OrdererDiagnosticEvent>& log) {
    std::ostringstream oss;
    for (const auto& e : log) {
        oss << "{\"packet_id\":" << e.packet_id
            << ",\"sequence_id\":" << e.sequence_id
            << ",\"decision\":\"" << ptsp::to_string(e.decision)
            << "\",\"error_code\":\"" << ptsp::to_string(e.error_code)
            << "\",\"reason\":\"" << ptsp::json_escape(e.reason) << "\"}\n";
    }
    return oss.str();
}

int cmd_probe(int argc, char** argv) {
    std::string iface = "eth0";
    std::string ptp_dev = "/dev/ptp0";
    std::string out_path;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--interface" && i + 1 < argc) {
            iface = argv[++i];
        } else if (arg == "--ptp-device" && i + 1 < argc) {
            ptp_dev = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    ptsp::SoftwareTimestampAdapter sw("host-sw-clock", 250);
    ptsp::LinuxPtpHardwareAdapter hw(iface, ptp_dev);
    const auto sw_rep = sw.probe_capability();
    const auto hw_rep = hw.probe_capability();

    std::ostringstream combined;
    combined << "{\n"
             << "  \"schema_version\": \"" << ptsp::kSchemaVersion << "\",\n"
             << "  \"error_source_note\": \"Direct host probes (MEASURED); unavailable hardware capabilities are explicitly reported and never fabricated.\",\n"
             << "  \"software_adapter\": " << ptsp::serialize_capability_report_json(sw_rep) << ",\n"
             << "  \"linux_ptp_hw_adapter\": " << ptsp::serialize_capability_report_json(hw_rep)
             << "}\n";

    std::cout << combined.str();
    if (!out_path.empty()) {
        auto wr = ptsp::write_text_file(out_path, combined.str());
        if (!wr.ok()) {
            std::cerr << "Error writing probe report: " << wr.message << "\n";
            return 1;
        }
    }
    return 0;
}

int cmd_replay(int argc, char** argv) {
    std::string cfg_path;
    std::string out_path;
    std::string audit_path;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--audit-out" && i + 1 < argc) {
            audit_path = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (cfg_path.empty()) {
        std::cerr << "Missing required --config <path>\n";
        return 2;
    }

    ptsp::ScenarioConfig cfg{};
    auto cfg_res = ptsp::load_scenario_config_kv(cfg_path, cfg);
    if (!cfg_res.ok()) {
        std::cerr << "[" << ptsp::to_string(cfg_res.code) << "] " << cfg_res.message << "\n";
        return 1;
    }

    namespace fs = std::filesystem;
    const fs::path base_dir = fs::path(cfg_path).parent_path();
    auto resolve = [&](const std::string& p) -> std::string {
        if (p.empty()) return {};
        fs::path fp(p);
        if (fp.is_absolute()) return fp.string();
        return (base_dir / fp).lexically_normal().string();
    };

    std::vector<ptsp::SyncExchangeSample> sync_samples;
    if (!cfg.sync_fixture_path.empty()) {
        auto s_res = ptsp::load_sync_samples_csv(resolve(cfg.sync_fixture_path), sync_samples);
        if (!s_res.ok()) {
            std::cerr << "[" << ptsp::to_string(s_res.code) << "] " << s_res.message << "\n";
            return 1;
        }
    }

    std::vector<ptsp::TimestampedPacket> packets;
    if (!cfg.packet_fixture_path.empty()) {
        auto p_res = ptsp::load_packets_csv(resolve(cfg.packet_fixture_path), packets);
        if (!p_res.ok()) {
            std::cerr << "[" << ptsp::to_string(p_res.code) << "] " << p_res.message << "\n";
            return 1;
        }
    }

    ptsp::ScenarioEvaluationSummary summary{};
    auto run_res = ptsp::run_scenario(cfg, sync_samples, packets, summary);
    if (!run_res.ok()) {
        std::cerr << "[" << ptsp::to_string(run_res.code) << "] " << run_res.message << "\n";
        return 1;
    }

    const std::string json = ptsp::serialize_scenario_summary_json(summary);
    std::cout << json;
    if (!out_path.empty()) {
        auto wr = ptsp::write_text_file(out_path, json);
        if (!wr.ok()) {
            std::cerr << "Failed to write report: " << wr.message << "\n";
            return 1;
        }
    }
    if (!audit_path.empty()) {
        auto wr = ptsp::write_text_file(audit_path, serialize_audit_jsonl(summary.audit_log));
        if (!wr.ok()) {
            std::cerr << "Failed to write audit log: " << wr.message << "\n";
            return 1;
        }
    }
    return 0;
}

int cmd_demo(int argc, char** argv) {
    std::string fixtures_dir = "fixtures";
    std::string reports_dir = "reports";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--fixtures-dir" && i + 1 < argc) {
            fixtures_dir = argv[++i];
        } else if (arg == "--reports-dir" && i + 1 < argc) {
            reports_dir = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    namespace fs = std::filesystem;
    const std::vector<std::string> scenarios = {
        "reference_drift",
        "clock_step_and_outliers",
        "packet_asymmetry",
        "clock_restart",
        "out_of_order_stream",
        "skew_violation_fail_closed",
    };

    // 1) Host capability probe (MEASURED).
    ptsp::SoftwareTimestampAdapter sw("host-sw-clock", 250);
    ptsp::LinuxPtpHardwareAdapter hw("eth0", "/dev/ptp0");
    const auto sw_rep = sw.probe_capability();
    const auto hw_rep = hw.probe_capability();

    std::ostringstream probe_json;
    probe_json << "{\n"
               << "  \"schema_version\": \"" << ptsp::kSchemaVersion << "\",\n"
               << "  \"software_adapter\": " << ptsp::serialize_capability_report_json(sw_rep) << ",\n"
               << "  \"linux_ptp_hw_adapter\": " << ptsp::serialize_capability_report_json(hw_rep)
               << "}\n";
    auto pw = ptsp::write_text_file((fs::path(reports_dir) / "host_probe.json").string(), probe_json.str());
    if (!pw.ok()) {
        std::cerr << "Failed writing host_probe.json: " << pw.message << "\n";
        return 1;
    }

    // 2) Run all versioned synthetic scenarios (SYNTHETIC_FIXTURE).
    std::ostringstream bundle;
    bundle << "{\n"
           << "  \"schema_version\": \"" << ptsp::kSchemaVersion << "\",\n"
           << "  \"error_source\": \"SYNTHETIC_FIXTURE\",\n"
           << "  \"scenarios\": [\n";

    for (std::size_t idx = 0; idx < scenarios.size(); ++idx) {
        const std::string& name = scenarios[idx];
        const fs::path cfg_path = fs::path(fixtures_dir) / (name + ".cfg");
        ptsp::ScenarioConfig cfg{};
        auto cfg_res = ptsp::load_scenario_config_kv(cfg_path.string(), cfg);
        if (!cfg_res.ok()) {
            std::cerr << "Scenario '" << name << "' config error: " << cfg_res.message << "\n";
            return 1;
        }
        std::vector<ptsp::SyncExchangeSample> sync_samples;
        std::vector<ptsp::TimestampedPacket> packets;
        auto s_res = ptsp::load_sync_samples_csv((fs::path(fixtures_dir) / cfg.sync_fixture_path).string(), sync_samples);
        if (!s_res.ok()) {
            std::cerr << "Scenario '" << name << "' sync fixture error: " << s_res.message << "\n";
            return 1;
        }
        auto p_res = ptsp::load_packets_csv((fs::path(fixtures_dir) / cfg.packet_fixture_path).string(), packets);
        if (!p_res.ok()) {
            std::cerr << "Scenario '" << name << "' packet fixture error: " << p_res.message << "\n";
            return 1;
        }
        ptsp::ScenarioEvaluationSummary summary{};
        auto r_res = ptsp::run_scenario(cfg, sync_samples, packets, summary);
        if (!r_res.ok()) {
            std::cerr << "Scenario '" << name << "' execution error: " << r_res.message << "\n";
            return 1;
        }

        const std::string json = ptsp::serialize_scenario_summary_json(summary);
        auto w_rep = ptsp::write_text_file((fs::path(reports_dir) / ("scenario_" + name + ".json")).string(), json);
        if (!w_rep.ok()) {
            std::cerr << "Failed writing scenario report: " << w_rep.message << "\n";
            return 1;
        }
        auto w_aud = ptsp::write_text_file(
            (fs::path(reports_dir) / ("audit_" + name + ".jsonl")).string(),
            serialize_audit_jsonl(summary.audit_log)
        );
        if (!w_aud.ok()) {
            std::cerr << "Failed writing audit log: " << w_aud.message << "\n";
            return 1;
        }

        bundle << json << (idx + 1 < scenarios.size() ? ",\n" : "\n");

        std::cout << "[DEMO] scenario=" << name
                  << " fingerprint=" << summary.run_fingerprint
                  << " baseline_mae_ns=" << summary.baseline_mae_offset_ns
                  << " robust_mae_ns=" << summary.robust_mae_offset_ns
                  << " steady_max_err_ns=" << summary.robust_steady_state_max_error_ns
                  << " orderer_state=" << ptsp::to_string(summary.final_orderer_state)
                  << " emitted=" << summary.orderer_counters.packets_emitted
                  << " reordered=" << summary.orderer_counters.packets_reordered
                  << "\n";
    }

    bundle << "  ]\n}\n";
    auto bw = ptsp::write_text_file((fs::path(reports_dir) / "demo_scenarios_summary.json").string(), bundle.str());
    if (!bw.ok()) {
        std::cerr << "Failed writing demo_scenarios_summary.json: " << bw.message << "\n";
        return 1;
    }

    std::cout << "[PROBE] software_adapter available=" << (sw_rep.available ? "true" : "false")
              << " median_read_ns=" << sw_rep.median_read_latency_ns
              << " p99_read_ns=" << sw_rep.p99_read_latency_ns
              << " source=" << ptsp::to_string(sw_rep.error_source) << "\n";
    std::cout << "[PROBE] linux_ptp_hw_adapter available=" << (hw_rep.available ? "true" : "false")
              << " status=" << ptsp::to_string(hw_rep.status_code)
              << " (" << hw_rep.status_message << ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "probe") return cmd_probe(argc, argv);
    if (cmd == "replay") return cmd_replay(argc, argv);
    if (cmd == "demo") return cmd_demo(argc, argv);
    print_usage();
    return 2;
}
