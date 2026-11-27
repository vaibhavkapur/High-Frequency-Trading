#include "kbnl/config.hpp"
#include "kbnl/protocol.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <variant>

namespace kbnl {

namespace {

// Minimal strict JSON parser supporting objects, strings, integers, booleans, null.
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    enum class Type { kNull, kBool, kInt, kString, kObject };
    Type type = Type::kNull;
    bool bool_val = false;
    int64_t int_val = 0;
    std::string str_val{};
    JsonObject obj_val{};
};

class MiniJsonParser {
public:
    explicit MiniJsonParser(const std::string& s) : s_(s) {}

    bool parse(JsonValue& out, std::string& err) {
        skip_ws();
        if (!parse_value(out, err)) {
            return false;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            err = "Unexpected trailing characters at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    bool parse_value(JsonValue& out, std::string& err) {
        skip_ws();
        if (pos_ >= s_.size()) {
            err = "Unexpected end of JSON input";
            return false;
        }
        const char c = s_[pos_];
        if (c == '{') return parse_object(out, err);
        if (c == '"') return parse_string_val(out, err);
        if (c == 't' || c == 'f') return parse_bool(out, err);
        if (c == 'n') return parse_null(out, err);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_int(out, err);
        err = std::string("Unsupported or invalid JSON token starting with '") + c + "'";
        return false;
    }

    bool parse_object(JsonValue& out, std::string& err) {
        out = JsonValue{};
        out.type = JsonValue::Type::kObject;
        ++pos_; // '{'
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            skip_ws();
            std::string key;
            if (!parse_raw_string(key, err)) {
                return false;
            }
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                err = "Expected ':' after key '" + key + "'";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err)) {
                return false;
            }
            out.obj_val[key] = std::move(val);
            skip_ws();
            if (pos_ >= s_.size()) {
                err = "Unterminated JSON object";
                return false;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') {
                err = "Expected ',' or '}' in JSON object";
                return false;
            }
            ++pos_;
        }
        err = "Unterminated JSON object";
        return false;
    }

    bool parse_raw_string(std::string& out, std::string& err) {
        if (pos_ >= s_.size() || s_[pos_] != '"') {
            err = "Expected string opening quote";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= s_.size()) {
                    err = "Unterminated escape sequence";
                    return false;
                }
                char esc = s_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default:
                        err = std::string("Unsupported escape '\\") + esc + "'";
                        return false;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "Unterminated JSON string";
        return false;
    }

    bool parse_string_val(JsonValue& out, std::string& err) {
        out = JsonValue{};
        out.type = JsonValue::Type::kString;
        return parse_raw_string(out.str_val, err);
    }

    bool parse_bool(JsonValue& out, std::string& err) {
        out = JsonValue{};
        out.type = JsonValue::Type::kBool;
        if (s_.compare(pos_, 4, "true") == 0) {
            out.bool_val = true;
            pos_ += 4;
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            out.bool_val = false;
            pos_ += 5;
            return true;
        }
        err = "Invalid boolean literal";
        return false;
    }

    bool parse_null(JsonValue& out, std::string& err) {
        if (s_.compare(pos_, 4, "null") == 0) {
            out = JsonValue{};
            out.type = JsonValue::Type::kNull;
            pos_ += 4;
            return true;
        }
        err = "Invalid null literal";
        return false;
    }

    bool parse_int(JsonValue& out, std::string& err) {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            err = "Invalid integer literal";
            return false;
        }
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            err = "Floating point numbers are not accepted for integer config fields";
            return false;
        }
        try {
            out = JsonValue{};
            out.type = JsonValue::Type::kInt;
            out.int_val = std::stoll(s_.substr(start, pos_ - start));
            return true;
        } catch (...) {
            err = "Integer out of range";
            return false;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool is_valid_ipv4(const std::string& ip) {
    in_addr addr{};
    return ::inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool is_power_of_two(size_t v) noexcept {
    return v >= 2 && (v & (v - 1)) == 0;
}

void apply_object_fields(
    const JsonObject& obj,
    BenchmarkConfig& cfg,
    std::vector<ValidationError>& errors) {
    auto read_str = [&](const std::string& key, std::string& dst) {
        auto it = obj.find(key);
        if (it == obj.end()) return;
        if (it->second.type != JsonValue::Type::kString) {
            errors.push_back({key, "TYPE_ERROR", "Expected string for field " + key});
            return;
        }
        dst = it->second.str_val;
    };
    auto read_bool = [&](const std::string& key, bool& dst) {
        auto it = obj.find(key);
        if (it == obj.end()) return;
        if (it->second.type != JsonValue::Type::kBool) {
            errors.push_back({key, "TYPE_ERROR", "Expected boolean for field " + key});
            return;
        }
        dst = it->second.bool_val;
    };
    auto read_i64 = [&](const std::string& key, auto& dst, bool allow_negative) {
        auto it = obj.find(key);
        if (it == obj.end()) return;
        if (it->second.type != JsonValue::Type::kInt) {
            errors.push_back({key, "TYPE_ERROR", "Expected integer for field " + key});
            return;
        }
        if (!allow_negative && it->second.int_val < 0) {
            errors.push_back({key, "RANGE_ERROR", "Field " + key + " must be non-negative"});
            return;
        }
        dst = static_cast<std::decay_t<decltype(dst)>>(it->second.int_val);
    };

    read_i64("schema_version", cfg.workload.schema_version, false);
    read_str("scenario_name", cfg.workload.scenario_name);
    read_i64("feed_id", cfg.workload.feed_id, false);
    read_i64("seed", cfg.workload.seed, false);
    read_i64("packet_bytes", cfg.workload.packet_bytes, false);
    read_i64("warmup_packets", cfg.workload.warmup_packets, false);
    read_i64("measured_packets", cfg.workload.measured_packets, false);
    read_i64("target_pps", cfg.workload.target_pps, false);
    read_i64("burst_size", cfg.workload.burst_size, false);
    read_i64("burst_interval_us", cfg.workload.burst_interval_us, false);
    read_i64("drain_timeout_ms", cfg.workload.drain_timeout_ms, false);
    read_i64("batch_size", cfg.workload.batch_size, false);

    if (auto it = obj.find("pacing_mode"); it != obj.end()) {
        if (it->second.type != JsonValue::Type::kString) {
            errors.push_back({"pacing_mode", "TYPE_ERROR", "Expected string for pacing_mode"});
        } else if (!parse_pacing_mode(it->second.str_val, cfg.workload.pacing_mode)) {
            errors.push_back({"pacing_mode", "UNSUPPORTED_VALUE",
                              "Unsupported pacing_mode: '" + it->second.str_val + "'"});
        }
    }

    if (auto it = obj.find("backend"); it != obj.end()) {
        if (it->second.type != JsonValue::Type::kString) {
            errors.push_back({"backend", "TYPE_ERROR", "Expected string for backend"});
        } else if (!parse_backend_name(it->second.str_val, cfg.workload.backend)) {
            errors.push_back({"backend", "UNSUPPORTED_BACKEND",
                              "Unsupported backend: '" + it->second.str_val + "'"});
        }
    }

    read_str("bind_ip", cfg.topology.bind_ip);
    read_str("target_ip", cfg.topology.target_ip);
    read_i64("port", cfg.topology.port, false);
    read_str("interface_name", cfg.topology.interface_name);
    read_i64("rx_queue_id", cfg.topology.rx_queue_id, false);
    read_i64("sender_cpu", cfg.topology.sender_cpu, true);
    read_i64("receiver_cpu", cfg.topology.receiver_cpu, true);
    read_bool("pin_threads", cfg.topology.pin_threads);
    read_bool("strict_pinning", cfg.topology.strict_pinning);
    read_i64("socket_rcvbuf_bytes", cfg.topology.socket_rcvbuf_bytes, false);
    read_i64("socket_sndbuf_bytes", cfg.topology.socket_sndbuf_bytes, false);
    read_i64("ring_capacity", cfg.topology.ring_capacity, false);
    read_str("output_path", cfg.output_path);
    read_str("fixture_path", cfg.fixture_path);
}

} // namespace

std::string_view backend_name(TransportBackend backend) noexcept {
    switch (backend) {
        case TransportBackend::kPosixUdpBlocking: return "posix_udp_blocking";
        case TransportBackend::kPosixUdpBusyPoll: return "posix_udp_busy_poll";
        case TransportBackend::kPosixUdpRecvmmsg: return "posix_udp_recvmmsg";
        case TransportBackend::kAfXdp: return "af_xdp";
        case TransportBackend::kDpdk: return "dpdk";
    }
    return "unknown";
}

bool parse_backend_name(std::string_view name, TransportBackend& out) noexcept {
    if (name == "posix_udp_blocking" || name == "posix_udp") {
        out = TransportBackend::kPosixUdpBlocking;
        return true;
    }
    if (name == "posix_udp_busy_poll") {
        out = TransportBackend::kPosixUdpBusyPoll;
        return true;
    }
    if (name == "posix_udp_recvmmsg") {
        out = TransportBackend::kPosixUdpRecvmmsg;
        return true;
    }
    if (name == "af_xdp") {
        out = TransportBackend::kAfXdp;
        return true;
    }
    if (name == "dpdk") {
        out = TransportBackend::kDpdk;
        return true;
    }
    return false;
}

std::string_view pacing_mode_name(PacingMode mode) noexcept {
    switch (mode) {
        case PacingMode::kConstantRate: return "constant_rate";
        case PacingMode::kBurst: return "burst";
    }
    return "unknown";
}

bool parse_pacing_mode(std::string_view name, PacingMode& out) noexcept {
    if (name == "constant_rate") {
        out = PacingMode::kConstantRate;
        return true;
    }
    if (name == "burst") {
        out = PacingMode::kBurst;
        return true;
    }
    return false;
}

std::vector<ValidationError> validate_config(const BenchmarkConfig& config) {
    std::vector<ValidationError> errors;
    const auto& w = config.workload;
    const auto& t = config.topology;

    if (w.schema_version != 1) {
        errors.push_back({"schema_version", "UNSUPPORTED_SCHEMA",
                          "Only schema_version 1 is supported"});
    }
    if (w.scenario_name.empty()) {
        errors.push_back({"scenario_name", "EMPTY_VALUE", "scenario_name must not be empty"});
    }
    if (w.packet_bytes < kMinPacketBytes || w.packet_bytes > kMaxPacketBytes) {
        errors.push_back({"packet_bytes", "RANGE_ERROR",
                          "packet_bytes must be within [64, 1472]"});
    }
    if (w.measured_packets == 0) {
        errors.push_back({"measured_packets", "RANGE_ERROR",
                          "measured_packets must be >= 1"});
    }
    if (w.target_pps == 0) {
        errors.push_back({"target_pps", "RANGE_ERROR",
                          "target_pps must be >= 1"});
    }
    if (w.pacing_mode == PacingMode::kBurst) {
        if (w.burst_size < 2) {
            errors.push_back({"burst_size", "RANGE_ERROR",
                              "burst_size must be >= 2 when pacing_mode is 'burst'"});
        }
        if (w.burst_interval_us == 0) {
            errors.push_back({"burst_interval_us", "RANGE_ERROR",
                              "burst_interval_us must be >= 1 when pacing_mode is 'burst'"});
        }
    }
    if (w.batch_size == 0 || w.batch_size > 1024) {
        errors.push_back({"batch_size", "RANGE_ERROR",
                          "batch_size must be within [1, 1024]"});
    }
    if (w.drain_timeout_ms == 0 || w.drain_timeout_ms > 60000) {
        errors.push_back({"drain_timeout_ms", "RANGE_ERROR",
                          "drain_timeout_ms must be within [1, 60000]"});
    }
    if (!is_valid_ipv4(t.bind_ip)) {
        errors.push_back({"bind_ip", "INVALID_IP", "bind_ip is not a valid IPv4 address"});
    }
    if (!is_valid_ipv4(t.target_ip)) {
        errors.push_back({"target_ip", "INVALID_IP", "target_ip is not a valid IPv4 address"});
    }
    if (t.interface_name.empty()) {
        errors.push_back({"interface_name", "EMPTY_VALUE", "interface_name must not be empty"});
    }
    if (t.sender_cpu < -1 || t.receiver_cpu < -1) {
        errors.push_back({"cpu_pin", "RANGE_ERROR",
                          "sender_cpu and receiver_cpu must be >= -1 (-1 means unpinned)"});
    }
    if (t.pin_threads && t.sender_cpu < 0 && t.receiver_cpu < 0) {
        errors.push_back({"pin_threads", "INCONSISTENT_CONFIG",
                          "pin_threads=true requires sender_cpu >= 0 or receiver_cpu >= 0"});
    }
    if (t.socket_rcvbuf_bytes < 4096 || t.socket_sndbuf_bytes < 4096) {
        errors.push_back({"socket_buf_bytes", "RANGE_ERROR",
                          "socket_rcvbuf_bytes and socket_sndbuf_bytes must be >= 4096"});
    }
    if (!is_power_of_two(t.ring_capacity)) {
        errors.push_back({"ring_capacity", "INVALID_CAPACITY",
                          "ring_capacity must be a power of two >= 2"});
    }
    return errors;
}

bool load_config_from_json_string(
    const std::string& json_text,
    BenchmarkConfig& out_config,
    std::vector<ValidationError>& out_errors) {
    out_errors.clear();
    MiniJsonParser parser(json_text);
    JsonValue root;
    std::string parse_err;
    if (!parser.parse(root, parse_err)) {
        out_errors.push_back({"$root", "JSON_PARSE_ERROR", parse_err});
        return false;
    }
    if (root.type != JsonValue::Type::kObject) {
        out_errors.push_back({"$root", "TYPE_ERROR", "Top-level JSON must be an object"});
        return false;
    }

    apply_object_fields(root.obj_val, out_config, out_errors);
    if (auto it = root.obj_val.find("workload"); it != root.obj_val.end()) {
        if (it->second.type == JsonValue::Type::kObject) {
            apply_object_fields(it->second.obj_val, out_config, out_errors);
        } else {
            out_errors.push_back({"workload", "TYPE_ERROR", "workload must be a JSON object"});
        }
    }
    if (auto it = root.obj_val.find("topology"); it != root.obj_val.end()) {
        if (it->second.type == JsonValue::Type::kObject) {
            apply_object_fields(it->second.obj_val, out_config, out_errors);
        } else {
            out_errors.push_back({"topology", "TYPE_ERROR", "topology must be a JSON object"});
        }
    }

    auto semantic_errors = validate_config(out_config);
    out_errors.insert(out_errors.end(), semantic_errors.begin(), semantic_errors.end());
    return out_errors.empty();
}

bool load_config_from_json_file(
    const std::string& path,
    BenchmarkConfig& out_config,
    std::vector<ValidationError>& out_errors) {
    out_errors.clear();
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        out_errors.push_back({"config_path", "FILE_NOT_FOUND", "Unable to open config file: " + path});
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return load_config_from_json_string(oss.str(), out_config, out_errors);
}

std::string compute_config_hash(const BenchmarkConfig& config) {
    std::ostringstream oss;
    const auto& w = config.workload;
    const auto& t = config.topology;
    oss << w.schema_version << "|" << w.scenario_name << "|" << w.feed_id << "|"
        << w.seed << "|" << w.packet_bytes << "|" << w.warmup_packets << "|"
        << w.measured_packets << "|" << w.target_pps << "|"
        << pacing_mode_name(w.pacing_mode) << "|" << w.burst_size << "|"
        << w.burst_interval_us << "|" << w.drain_timeout_ms << "|"
        << w.batch_size << "|" << backend_name(w.backend) << "|"
        << t.bind_ip << "|" << t.target_ip << "|" << t.port << "|"
        << t.interface_name << "|" << t.rx_queue_id << "|" << t.sender_cpu << "|"
        << t.receiver_cpu << "|" << (t.pin_threads ? 1 : 0) << "|"
        << (t.strict_pinning ? 1 : 0) << "|" << t.socket_rcvbuf_bytes << "|"
        << t.socket_sndbuf_bytes << "|" << t.ring_capacity;

    const std::string canonical = oss.str();
    uint64_t hash = 14695981039346656037ULL;
    for (char ch : canonical) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    std::ostringstream hex;
    hex << std::hex << std::setfill('0') << std::setw(16) << hash;
    return hex.str();
}

} // namespace kbnl
