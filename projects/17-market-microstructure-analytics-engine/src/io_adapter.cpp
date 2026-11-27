#include "mmae/io_adapter.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace mmae {

namespace {

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool parse_int64_strict(const std::string& s, int64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    try {
        size_t pos = 0;
        long long v = std::stoll(t, &pos, 10);
        if (pos != t.size()) return false;
        out = static_cast<int64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_uint64_strict(const std::string& s, uint64_t& out) {
    std::string t = trim(s);
    if (t.empty() || t[0] == '-') return false;
    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(t, &pos, 10);
        if (pos != t.size()) return false;
        out = static_cast<uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

std::string json_escape(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
    return oss.str();
}

void write_opt_double(std::ostream& os, double v, int precision = 8) {
    if (std::isfinite(v)) {
        os << std::fixed << std::setprecision(precision) << v;
    }
}

bool extract_json_value(const std::string& text, const std::string& key, std::string& out_val) {
    std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
    }
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) return false;
        out_val = text.substr(pos + 1, end - pos - 1);
        return true;
    }
    size_t end = pos;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n' && text[end] != '\r') {
        ++end;
    }
    out_val = trim(text.substr(pos, end - pos));
    return !out_val.empty();
}

}  // namespace

bool parse_event_csv_line(const std::string& line, size_t line_number, ParsedRow& out) {
    out = ParsedRow{};
    out.line_number = line_number;

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        cols.push_back(trim(item));
    }
    if (!line.empty() && line.back() == ',') {
        cols.emplace_back("");
    }

    if (cols.size() != 9) {
        out.ok = false;
        out.error = ValidationErrorCode::MALFORMED_ROW;
        out.error_detail = "Expected 9 CSV columns, got " + std::to_string(cols.size());
        return false;
    }

    Event ev{};
    if (!parse_int64_strict(cols[0], ev.event_ts_ns) || !parse_int64_strict(cols[1], ev.recv_ts_ns)) {
        out.ok = false;
        out.error = ValidationErrorCode::INVALID_TIMESTAMP;
        out.error_detail = "Unparseable event_ts_ns or recv_ts_ns";
        return false;
    }
    if (!parse_uint64_strict(cols[2], ev.seq_num)) {
        out.ok = false;
        out.error = ValidationErrorCode::MALFORMED_ROW;
        out.error_detail = "Unparseable seq_num";
        return false;
    }
    ev.symbol = cols[3];
    if (!parse_event_type(cols[4], ev.type)) {
        out.ok = false;
        out.error = ValidationErrorCode::UNKNOWN_EVENT_TYPE;
        out.error_detail = "Unknown event_type: " + cols[4];
        return false;
    }
    if (!parse_side(cols[5], ev.side)) {
        out.ok = false;
        out.error = ValidationErrorCode::INVALID_SIDE;
        out.error_detail = "Invalid side: " + cols[5];
        return false;
    }
    if (!parse_int64_strict(cols[6], ev.price_ticks)) {
        out.ok = false;
        out.error = ValidationErrorCode::MALFORMED_ROW;
        out.error_detail = "Unparseable price_ticks";
        return false;
    }
    if (!parse_int64_strict(cols[7], ev.qty)) {
        out.ok = false;
        out.error = ValidationErrorCode::MALFORMED_ROW;
        out.error_detail = "Unparseable qty";
        return false;
    }
    if (!parse_uint64_strict(cols[8], ev.order_id)) {
        out.ok = false;
        out.error = ValidationErrorCode::MALFORMED_ROW;
        out.error_detail = "Unparseable order_id";
        return false;
    }

    out.ok = true;
    out.event = std::move(ev);
    return true;
}

bool load_events_csv(const std::string& path, std::vector<ParsedRow>& out_rows, std::string& io_error) {
    out_rows.clear();
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        io_error = "Failed to open input CSV file: " + path;
        return false;
    }

    std::string header;
    if (!std::getline(ifs, header)) {
        io_error = "Input CSV file is empty: " + path;
        return false;
    }
    std::string expected_header =
        "event_ts_ns,recv_ts_ns,seq_num,symbol,event_type,side,price_ticks,qty,order_id";
    if (trim(header) != expected_header) {
        io_error = "Invalid CSV header. Expected '" + expected_header + "', got '" + trim(header) + "'";
        return false;
    }

    std::string line;
    size_t line_no = 1;
    while (std::getline(ifs, line)) {
        ++line_no;
        if (trim(line).empty()) continue;
        ParsedRow pr{};
        parse_event_csv_line(line, line_no, pr);
        out_rows.push_back(std::move(pr));
    }
    return true;
}

bool load_config_json(const std::string& path, EngineConfig& config, std::string& error_out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        error_out = "Failed to open config JSON file: " + path;
        return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string text = buf.str();
    if (trim(text).empty() || text.find('{') == std::string::npos || text.find('}') == std::string::npos) {
        error_out = "Malformed JSON config file: " + path;
        return false;
    }

    std::string val;
    try {
        if (extract_json_value(text, "tick_size", val)) config.tick_size = std::stod(val);
        if (extract_json_value(text, "stale_threshold_ns", val)) config.stale_threshold_ns = std::stoll(val);
        if (extract_json_value(text, "flow_window_ns", val)) config.flow_window_ns = std::stoll(val);
        if (extract_json_value(text, "depth_levels_k", val)) config.depth_levels_k = std::stoull(val);
        if (extract_json_value(text, "max_levels", val)) config.max_levels = std::stoull(val);
        if (extract_json_value(text, "max_orders", val)) config.max_orders = std::stoull(val);
        if (extract_json_value(text, "clear_gap_on_snapshot", val)) {
            if (val == "true") config.clear_gap_on_snapshot = true;
            else if (val == "false") config.clear_gap_on_snapshot = false;
            else {
                error_out = "Invalid boolean for clear_gap_on_snapshot: " + val;
                return false;
            }
        }
        if (extract_json_value(text, "variant", val)) config.variant = val;
    } catch (const std::exception& ex) {
        error_out = std::string("Failed parsing config JSON numeric field: ") + ex.what();
        return false;
    }

    return validate_config(config, error_out);
}

void write_features_csv_header(std::ostream& os) {
    os << "seq_num,event_ts_ns,recv_ts_ns,symbol,event_type,"
       << "book_valid,is_crossed,is_locked,is_stale,gap_active,gap_detected_here,"
       << "best_bid_ticks,best_ask_ticks,best_bid_px,best_ask_px,"
       << "mid_price,spread_ticks,spread_abs,spread_bps,microprice,microprice_offset_ticks,"
       << "bid_qty_l1,ask_qty_l1,bid_depth_k,ask_depth_k,obi_l1,obi_k,ofi_event,ofi_window,"
       << "is_trade,trade_price_ticks,trade_price,trade_qty,trade_sign,classification_reason,"
       << "is_ambiguous,tick_rule_hint,signed_qty,"
       << "rolling_signed_flow_qty,rolling_total_trade_qty,rolling_ambiguous_qty,rolling_trade_flow_imbalance,"
       << "bid_l1_order_count,ask_l1_order_count,exec_queue_ahead_qty,exec_queue_ahead_orders,"
       << "exec_queue_position_ratio,fifo_anomaly,top_bid_depletion_qty,top_ask_depletion_qty,cancel_to_add_ratio\n";
}

void write_feature_csv_row(std::ostream& os, const FeatureRow& r) {
    os << r.seq_num << ','
       << r.event_ts_ns << ','
       << r.recv_ts_ns << ','
       << r.symbol << ','
       << to_string(r.event_type) << ','
       << (r.book_valid ? 1 : 0) << ','
       << (r.is_crossed ? 1 : 0) << ','
       << (r.is_locked ? 1 : 0) << ','
       << (r.is_stale ? 1 : 0) << ','
       << (r.gap_active ? 1 : 0) << ','
       << (r.gap_detected_here ? 1 : 0) << ',';

    if (r.has_bid) os << r.best_bid_ticks;
    os << ',';
    if (r.has_ask) os << r.best_ask_ticks;
    os << ',';
    write_opt_double(os, r.best_bid_px, 6);
    os << ',';
    write_opt_double(os, r.best_ask_px, 6);
    os << ',';
    write_opt_double(os, r.mid_price, 6);
    os << ',';
    if (r.has_bid && r.has_ask) os << r.spread_ticks;
    os << ',';
    write_opt_double(os, r.spread_abs, 6);
    os << ',';
    write_opt_double(os, r.spread_bps, 6);
    os << ',';
    write_opt_double(os, r.microprice, 6);
    os << ',';
    write_opt_double(os, r.microprice_offset_ticks, 6);
    os << ','
       << r.bid_qty_l1 << ','
       << r.ask_qty_l1 << ','
       << r.bid_depth_k << ','
       << r.ask_depth_k << ',';
    write_opt_double(os, r.obi_l1, 8);
    os << ',';
    write_opt_double(os, r.obi_k, 8);
    os << ',';
    write_opt_double(os, r.ofi_event, 6);
    os << ',';
    write_opt_double(os, r.ofi_window, 6);
    os << ','
       << (r.is_trade ? 1 : 0) << ',';
    if (r.is_trade) os << r.trade_price_ticks;
    os << ',';
    write_opt_double(os, r.trade_price, 6);
    os << ','
       << r.trade_qty << ',';
    if (r.is_trade) {
        os << static_cast<int>(r.trade_sign);
    }
    os << ','
       << to_string(r.classification_reason) << ','
       << (r.is_ambiguous ? 1 : 0) << ','
       << static_cast<int>(r.tick_rule_hint) << ','
       << r.signed_qty << ','
       << r.rolling_signed_flow_qty << ','
       << r.rolling_total_trade_qty << ','
       << r.rolling_ambiguous_qty << ',';
    write_opt_double(os, r.rolling_trade_flow_imbalance, 8);
    os << ','
       << r.bid_l1_order_count << ','
       << r.ask_l1_order_count << ',';
    if (r.exec_queue_ahead_qty >= 0) os << r.exec_queue_ahead_qty;
    os << ',';
    if (r.exec_queue_ahead_orders >= 0) os << r.exec_queue_ahead_orders;
    os << ',';
    write_opt_double(os, r.exec_queue_position_ratio, 8);
    os << ','
       << (r.fifo_anomaly ? 1 : 0) << ','
       << r.top_bid_depletion_qty << ','
       << r.top_ask_depletion_qty << ',';
    write_opt_double(os, r.cancel_to_add_ratio, 8);
    os << '\n';
}

bool write_features_csv_file(const std::string& path, const std::vector<FeatureRow>& rows, std::string& io_error) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        io_error = "Failed to open output features CSV for writing: " + path;
        return false;
    }
    write_features_csv_header(ofs);
    for (const auto& row : rows) {
        write_feature_csv_row(ofs, row);
    }
    ofs.flush();
    if (!ofs.good()) {
        io_error = "I/O error while writing features CSV: " + path;
        return false;
    }
    return true;
}

std::string diagnostics_to_json(const Diagnostics& d, const std::string& run_id) {
    std::ostringstream os;
    os << "{\n"
       << "  \"schema_version\": \"" << json_escape(d.schema_version) << "\",\n"
       << "  \"run_id\": \"" << json_escape(run_id) << "\",\n"
       << "  \"engine_variant\": \"" << json_escape(d.engine_variant) << "\",\n"
       << "  \"symbol\": \"" << json_escape(d.symbol) << "\",\n"
       << "  \"config\": {\n"
       << "    \"tick_size\": " << std::fixed << std::setprecision(8) << d.tick_size << ",\n"
       << "    \"stale_threshold_ns\": " << d.stale_threshold_ns << ",\n"
       << "    \"flow_window_ns\": " << d.flow_window_ns << ",\n"
       << "    \"depth_levels_k\": " << d.depth_levels_k << ",\n"
       << "    \"max_levels\": " << d.max_levels << ",\n"
       << "    \"max_orders\": " << d.max_orders << "\n"
       << "  },\n"
       << "  \"counts\": {\n"
       << "    \"total_input_rows\": " << d.total_input_rows << ",\n"
       << "    \"accepted_events\": " << d.accepted_events << ",\n"
       << "    \"rejected_events\": " << d.rejected_events << ",\n"
       << "    \"out_of_order_events\": " << d.out_of_order_events << ",\n"
       << "    \"duplicate_seq_events\": " << d.duplicate_seq_events << ",\n"
       << "    \"capacity_errors\": " << d.capacity_errors << ",\n"
       << "    \"unknown_order_errors\": " << d.unknown_order_errors << "\n"
       << "  },\n"
       << "  \"book_health\": {\n"
       << "    \"crossed_book_events\": " << d.crossed_book_events << ",\n"
       << "    \"locked_book_events\": " << d.locked_book_events << ",\n"
       << "    \"stale_book_events\": " << d.stale_book_events << ",\n"
       << "    \"gap_count\": " << d.gap_count << ",\n"
       << "    \"events_in_gap_state\": " << d.events_in_gap_state << "\n"
       << "  },\n"
       << "  \"trade_classification\": {\n"
       << "    \"total_trades\": " << d.total_trades << ",\n"
       << "    \"classified_buys\": " << d.classified_buys << ",\n"
       << "    \"classified_sells\": " << d.classified_sells << ",\n"
       << "    \"ambiguous_trades\": " << d.ambiguous_trades << ",\n"
       << "    \"ambiguous_midpoint\": " << d.ambiguous_midpoint << ",\n"
       << "    \"ambiguous_book_invalid\": " << d.ambiguous_book_invalid << ",\n"
       << "    \"ambiguous_stale\": " << d.ambiguous_stale << ",\n"
       << "    \"ambiguous_gap\": " << d.ambiguous_gap << "\n"
       << "  },\n"
       << "  \"queue_metrics\": {\n"
       << "    \"l3_adds\": " << d.l3_adds << ",\n"
       << "    \"l3_modifies\": " << d.l3_modifies << ",\n"
       << "    \"l3_cancels\": " << d.l3_cancels << ",\n"
       << "    \"l3_executes\": " << d.l3_executes << ",\n"
       << "    \"fifo_anomalies\": " << d.fifo_anomalies << "\n"
       << "  },\n"
       << "  \"gaps\": [";

    for (size_t i = 0; i < d.gaps.size(); ++i) {
        const auto& g = d.gaps[i];
        if (i > 0) os << ",";
        os << "\n    {"
           << "\"expected_seq\": " << g.expected_seq
           << ", \"observed_seq\": " << g.observed_seq
           << ", \"missing_count\": " << g.missing_count
           << ", \"prev_event_ts_ns\": " << g.prev_event_ts_ns
           << ", \"observed_event_ts_ns\": " << g.observed_event_ts_ns
           << ", \"resolved_by_snapshot\": " << (g.resolved_by_snapshot ? "true" : "false")
           << ", \"resolved_seq\": " << g.resolved_seq
           << "}";
    }
    if (!d.gaps.empty()) os << "\n  ";
    os << "],\n  \"validation_errors\": [";

    for (size_t i = 0; i < d.errors.size(); ++i) {
        const auto& e = d.errors[i];
        if (i > 0) os << ",";
        os << "\n    {"
           << "\"line_number\": " << e.line_number
           << ", \"seq_num\": " << e.seq_num
           << ", \"error_code\": \"" << to_string(e.code) << "\""
           << ", \"detail\": \"" << json_escape(e.detail) << "\""
           << "}";
    }
    if (!d.errors.empty()) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

bool write_diagnostics_json_file(
    const std::string& path, const Diagnostics& diag, const std::string& run_id, std::string& io_error
) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        io_error = "Failed to open diagnostics JSON for writing: " + path;
        return false;
    }
    ofs << diagnostics_to_json(diag, run_id);
    ofs.flush();
    if (!ofs.good()) {
        io_error = "I/O error while writing diagnostics JSON: " + path;
        return false;
    }
    return true;
}

}  // namespace mmae
