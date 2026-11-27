#include "kbnl/json_writer.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace kbnl {

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    out += oss.str();
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

namespace {

void write_pin_json(std::ostringstream& os, const PinResult& pin, const std::string& indent) {
    os << "{\n"
       << indent << "  \"requested\": " << (pin.requested ? "true" : "false") << ",\n"
       << indent << "  \"applied\": " << (pin.applied ? "true" : "false") << ",\n"
       << indent << "  \"requested_cpu\": " << pin.requested_cpu << ",\n"
       << indent << "  \"mechanism\": \"" << json_escape(pin.mechanism) << "\",\n"
       << indent << "  \"detail\": \"" << json_escape(pin.detail) << "\"\n"
       << indent << "}";
}

void write_clock_json(std::ostringstream& os, const ClockMetadata& c, const std::string& indent) {
    os << "{\n"
       << indent << "  \"name\": \"" << json_escape(c.name) << "\",\n"
       << indent << "  \"api\": \"" << json_escape(c.api) << "\",\n"
       << indent << "  \"nominal_resolution_ns\": " << c.nominal_resolution_ns << ",\n"
       << indent << "  \"measured_call_overhead_ns\": " << c.measured_call_overhead_ns << ",\n"
       << indent << "  \"is_monotonic\": " << (c.is_monotonic ? "true" : "false") << ",\n"
       << indent << "  \"same_host_domain_required\": " << (c.same_host_domain_required ? "true" : "false") << ",\n"
       << indent << "  \"notes\": \"" << json_escape(c.notes) << "\"\n"
       << indent << "}";
}

void write_capability_json(std::ostringstream& os, const CapabilityReport& cap, const std::string& indent) {
    os << "{\n"
       << indent << "  \"backend\": \"" << json_escape(cap.backend) << "\",\n"
       << indent << "  \"compiled\": " << (cap.compiled ? "true" : "false") << ",\n"
       << indent << "  \"os_supported\": " << (cap.os_supported ? "true" : "false") << ",\n"
       << indent << "  \"runtime_available\": " << (cap.runtime_available ? "true" : "false") << ",\n"
       << indent << "  \"status\": \"" << json_escape(cap.status) << "\",\n"
       << indent << "  \"reason_code\": \"" << status_code_name(cap.reason_code) << "\",\n"
       << indent << "  \"reason\": \"" << json_escape(cap.reason) << "\"\n"
       << indent << "}";
}

} // namespace

std::string serialize_capability_report_json(const CapabilityReport& report, const ClockMetadata& clock) {
    std::ostringstream os;
    os << "{\n"
       << "  \"schema_version\": 1,\n"
       << "  \"timestamp_utc\": \"" << json_escape(utc_timestamp_iso8601()) << "\",\n"
       << "  \"execution_status\": \"" << (report.runtime_available ? "AVAILABLE" : "NOT_RUN") << "\",\n"
       << "  \"is_simulated\": false,\n"
       << "  \"capability\": ";
    write_capability_json(os, report, "  ");
    os << ",\n  \"clock\": ";
    write_clock_json(os, clock, "  ");
    os << "\n}\n";
    return os.str();
}

std::string serialize_benchmark_result_json(const BenchmarkResult& r) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "{\n"
       << "  \"schema_version\": " << r.schema_version << ",\n"
       << "  \"run_id\": \"" << json_escape(r.run_id) << "\",\n"
       << "  \"config_hash\": \"" << json_escape(r.config_hash) << "\",\n"
       << "  \"timestamp_utc\": \"" << json_escape(r.timestamp_utc) << "\",\n"
       << "  \"scenario_name\": \"" << json_escape(r.scenario_name) << "\",\n"
       << "  \"backend\": \"" << json_escape(r.backend) << "\",\n"
       << "  \"execution_status\": \"" << json_escape(r.execution_status) << "\",\n"
       << "  \"is_simulated\": " << (r.is_simulated ? "true" : "false") << ",\n"
       << "  \"status\": {\n"
       << "    \"code\": \"" << status_code_name(r.status.code) << "\",\n"
       << "    \"message\": \"" << json_escape(r.status.message) << "\"\n"
       << "  },\n"
       << "  \"capability\": ";
    write_capability_json(os, r.capability, "  ");
    os << ",\n  \"clock\": ";
    write_clock_json(os, r.clock, "  ");
    os << ",\n  \"pinning\": {\n"
       << "    \"sender\": ";
    write_pin_json(os, r.sender_pin, "    ");
    os << ",\n    \"receiver\": ";
    write_pin_json(os, r.receiver_pin, "    ");
    os << "\n  },\n"
       << "  \"config\": {\n"
       << "    \"feed_id\": " << r.config.workload.feed_id << ",\n"
       << "    \"seed\": " << r.config.workload.seed << ",\n"
       << "    \"packet_bytes\": " << r.config.workload.packet_bytes << ",\n"
       << "    \"warmup_packets\": " << r.config.workload.warmup_packets << ",\n"
       << "    \"measured_packets\": " << r.config.workload.measured_packets << ",\n"
       << "    \"target_pps\": " << r.config.workload.target_pps << ",\n"
       << "    \"pacing_mode\": \"" << pacing_mode_name(r.config.workload.pacing_mode) << "\",\n"
       << "    \"burst_size\": " << r.config.workload.burst_size << ",\n"
       << "    \"burst_interval_us\": " << r.config.workload.burst_interval_us << ",\n"
       << "    \"batch_size\": " << r.config.workload.batch_size << ",\n"
       << "    \"drain_timeout_ms\": " << r.config.workload.drain_timeout_ms << ",\n"
       << "    \"bind_ip\": \"" << json_escape(r.config.topology.bind_ip) << "\",\n"
       << "    \"target_ip\": \"" << json_escape(r.config.topology.target_ip) << "\",\n"
       << "    \"interface_name\": \"" << json_escape(r.config.topology.interface_name) << "\",\n"
       << "    \"rx_queue_id\": " << r.config.topology.rx_queue_id << ",\n"
       << "    \"ring_capacity\": " << r.config.topology.ring_capacity << ",\n"
       << "    \"socket_rcvbuf_bytes\": " << r.config.topology.socket_rcvbuf_bytes << ",\n"
       << "    \"socket_sndbuf_bytes\": " << r.config.topology.socket_sndbuf_bytes << "\n"
       << "  },\n"
       << "  \"throughput\": {\n"
       << "    \"expected_warmup_sent\": " << r.expected_warmup_sent << ",\n"
       << "    \"expected_measured_sent\": " << r.expected_measured_sent << ",\n"
       << "    \"actual_packets_sent\": " << r.actual_packets_sent << ",\n"
       << "    \"send_errors\": " << r.send_errors << ",\n"
       << "    \"measured_duration_ns\": " << r.measured_duration_ns << ",\n"
       << "    \"achieved_send_pps\": " << r.achieved_send_pps << ",\n"
       << "    \"achieved_recv_pps\": " << r.achieved_recv_pps << ",\n"
       << "    \"achieved_recv_mbps\": " << r.achieved_recv_mbps << "\n"
       << "  },\n"
       << "  \"sequence\": {\n"
       << "    \"total_valid_data_packets\": " << r.sequence.total_valid_data_packets << ",\n"
       << "    \"unique_received\": " << r.sequence.unique_received << ",\n"
       << "    \"warmup_received\": " << r.sequence.warmup_received << ",\n"
       << "    \"measured_received\": " << r.sequence.measured_received << ",\n"
       << "    \"burst_packets\": " << r.sequence.burst_packets << ",\n"
       << "    \"gaps_events\": " << r.sequence.gaps_events << ",\n"
       << "    \"missing_within_highest\": " << r.sequence.missing_within_highest << ",\n"
       << "    \"inferred_total_lost\": " << r.inferred_total_lost << ",\n"
       << "    \"loss_ratio\": " << std::setprecision(6) << r.loss_ratio << std::setprecision(3) << ",\n"
       << "    \"out_of_order\": " << r.sequence.out_of_order << ",\n"
       << "    \"duplicates\": " << r.sequence.duplicates << ",\n"
       << "    \"too_old_out_of_window\": " << r.sequence.too_old_out_of_window << ",\n"
       << "    \"checksum_errors\": " << r.sequence.checksum_errors << ",\n"
       << "    \"decode_errors\": " << r.sequence.decode_errors << ",\n"
       << "    \"ring_drops\": " << r.sequence.ring_drops << ",\n"
       << "    \"eos_packets\": " << r.sequence.eos_packets << ",\n"
       << "    \"first_seq\": " << r.sequence.first_seq << ",\n"
       << "    \"highest_seq\": " << r.sequence.highest_seq << ",\n"
       << "    \"last_seq\": " << r.sequence.last_seq << ",\n"
       << "    \"accounting_invariant_holds\": " << (r.sequence.accounting_invariant_holds() ? "true" : "false") << ",\n"
       << "    \"sequence_accounting_verified\": " << (r.sequence_accounting_verified ? "true" : "false") << "\n"
       << "  },\n"
       << "  \"latency_ns\": {\n"
       << "    \"count\": " << r.latency.count << ",\n"
       << "    \"warmup_ignored\": " << r.latency.warmup_ignored << ",\n"
       << "    \"negative_clamped\": " << r.latency.negative_clamped << ",\n"
       << "    \"exact_quantiles\": " << (r.latency.exact_quantiles ? "true" : "false") << ",\n"
       << "    \"min\": " << r.latency.min_ns << ",\n"
       << "    \"mean\": " << r.latency.mean_ns << ",\n"
       << "    \"stddev\": " << r.latency.stddev_ns << ",\n"
       << "    \"p50\": " << r.latency.p50_ns << ",\n"
       << "    \"p95\": " << r.latency.p95_ns << ",\n"
       << "    \"p99\": " << r.latency.p99_ns << ",\n"
       << "    \"p99_9\": " << r.latency.p999_ns << ",\n"
       << "    \"max\": " << r.latency.max_ns << "\n"
       << "  }\n"
       << "}\n";
    return os.str();
}

bool write_text_file(const std::string& path, const std::string& content, std::string& out_error) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out_error = "Failed to open file for writing: " + path;
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs.good()) {
        out_error = "Write error on file: " + path;
        return false;
    }
    return true;
}

} // namespace kbnl
