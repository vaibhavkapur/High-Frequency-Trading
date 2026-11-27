#include "obce/io_adapter.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace obce {

namespace {

const char* event_type_str(EventType t) noexcept {
    switch (t) {
        case EventType::AddOrder: return "ADD";
        case EventType::ModifyOrder: return "MODIFY";
        case EventType::CancelOrder: return "CANCEL";
        case EventType::ExecuteOrder: return "EXECUTE";
        case EventType::L2Update: return "L2_UPDATE";
        case EventType::L2Snapshot: return "L2_SNAPSHOT";
        case EventType::BookClear: return "BOOK_CLEAR";
    }
    return "UNKNOWN";
}

bool parse_event_type_str(const std::string& s, EventType& out) noexcept {
    if (s == "ADD") { out = EventType::AddOrder; return true; }
    if (s == "MODIFY") { out = EventType::ModifyOrder; return true; }
    if (s == "CANCEL") { out = EventType::CancelOrder; return true; }
    if (s == "EXECUTE") { out = EventType::ExecuteOrder; return true; }
    if (s == "L2_UPDATE") { out = EventType::L2Update; return true; }
    if (s == "L2_SNAPSHOT") { out = EventType::L2Snapshot; return true; }
    if (s == "BOOK_CLEAR") { out = EventType::BookClear; return true; }
    return false;
}

const char* side_str(Side s) noexcept {
    switch (s) {
        case Side::Unknown: return "NONE";
        case Side::Bid: return "BID";
        case Side::Ask: return "ASK";
    }
    return "NONE";
}

bool parse_side_str(const std::string& s, Side& out) noexcept {
    if (s == "NONE" || s == "UNKNOWN") { out = Side::Unknown; return true; }
    if (s == "BID") { out = Side::Bid; return true; }
    if (s == "ASK") { out = Side::Ask; return true; }
    return false;
}

const char* scope_str(BookScope sc) noexcept {
    return (sc == BookScope::L3) ? "L3" : "L2";
}

bool parse_scope_str(const std::string& s, BookScope& out) noexcept {
    if (s == "L2") { out = BookScope::L2; return true; }
    if (s == "L3") { out = BookScope::L3; return true; }
    return false;
}

// Simple flat-object JSON field extractor (for deterministic canonical JSONL fixtures)
bool extract_json_raw_value(const std::string& line, const std::string& key, std::string& val_out) {
    const std::string needle = "\"" + key + "\"";
    const size_t kpos = line.find(needle);
    if (kpos == std::string::npos) {
        return false;
    }
    size_t pos = line.find(':', kpos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos >= line.size()) {
        return false;
    }
    if (line[pos] == '"') {
        ++pos;
        const size_t end = line.find('"', pos);
        if (end == std::string::npos) return false;
        val_out = line.substr(pos, end - pos);
        return true;
    }
    size_t end = pos;
    while (end < line.size() && line[end] != ',' && line[end] != '}' &&
           !std::isspace(static_cast<unsigned char>(line[end]))) {
        ++end;
    }
    val_out = line.substr(pos, end - pos);
    return !val_out.empty();
}

}  // namespace

Result<std::vector<uint8_t>> read_binary_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::IoError, "Failed to open file for reading: " + path);
    }
    const std::streamsize size = ifs.tellg();
    if (size < 0) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::IoError, "Failed to query file size: " + path);
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !ifs.read(reinterpret_cast<char*>(data.data()), size)) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::IoError, "Failed to read file: " + path);
    }
    return Result<std::vector<uint8_t>>::Ok(std::move(data));
}

Status write_binary_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::Error(ErrorCode::IoError, "Failed to open file for writing: " + path);
    }
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!ofs.good()) {
        return Status::Error(ErrorCode::IoError, "Failed to write file: " + path);
    }
    return Status::Ok();
}

std::string event_to_json_line(const MarketEvent& ev, uint16_t schema_version) {
    std::ostringstream oss;
    oss << "{\"seq_id\":" << ev.seq_id
        << ",\"timestamp_ns\":" << ev.timestamp_ns
        << ",\"symbol_id\":" << ev.symbol_id
        << ",\"order_id\":" << ev.order_id
        << ",\"price_ticks\":" << ev.price_ticks
        << ",\"quantity\":" << ev.quantity
        << ",\"type\":\"" << event_type_str(ev.type) << "\""
        << ",\"side\":\"" << side_str(ev.side) << "\""
        << ",\"scope\":\"" << scope_str(ev.scope) << "\""
        << ",\"level_index\":" << ev.level_index;
    if (schema_version >= kSchemaVersionV2) {
        oss << ",\"venue_id\":" << ev.venue_id
            << ",\"flags\":" << ev.flags
            << ",\"match_id\":" << ev.match_id;
    }
    oss << "}";
    return oss.str();
}

Result<MarketEvent> parse_event_json_line(const std::string& line) {
    MarketEvent ev{};
    std::string raw;
    try {
        if (!extract_json_raw_value(line, "seq_id", raw)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Missing seq_id in JSONL");
        }
        ev.seq_id = std::stoull(raw);

        if (!extract_json_raw_value(line, "timestamp_ns", raw)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Missing timestamp_ns in JSONL");
        }
        ev.timestamp_ns = std::stoull(raw);

        if (!extract_json_raw_value(line, "symbol_id", raw)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Missing symbol_id in JSONL");
        }
        ev.symbol_id = static_cast<uint32_t>(std::stoul(raw));

        if (extract_json_raw_value(line, "order_id", raw)) {
            ev.order_id = std::stoull(raw);
        }
        if (!extract_json_raw_value(line, "price_ticks", raw)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Missing price_ticks in JSONL");
        }
        ev.price_ticks = std::stoll(raw);

        if (!extract_json_raw_value(line, "quantity", raw)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Missing quantity in JSONL");
        }
        ev.quantity = std::stoll(raw);

        if (!extract_json_raw_value(line, "type", raw) || !parse_event_type_str(raw, ev.type)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Invalid or missing type in JSONL");
        }
        if (!extract_json_raw_value(line, "side", raw) || !parse_side_str(raw, ev.side)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Invalid or missing side in JSONL");
        }
        if (!extract_json_raw_value(line, "scope", raw) || !parse_scope_str(raw, ev.scope)) {
            return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, "Invalid or missing scope in JSONL");
        }
        if (extract_json_raw_value(line, "level_index", raw)) {
            ev.level_index = static_cast<uint16_t>(std::stoul(raw));
        }
        if (extract_json_raw_value(line, "venue_id", raw)) {
            ev.venue_id = static_cast<uint16_t>(std::stoul(raw));
        }
        if (extract_json_raw_value(line, "flags", raw)) {
            ev.flags = static_cast<uint16_t>(std::stoul(raw));
        }
        if (extract_json_raw_value(line, "match_id", raw)) {
            ev.match_id = std::stoull(raw);
        }
    } catch (const std::exception& ex) {
        return Result<MarketEvent>::Err(ErrorCode::InvalidEvent, std::string("JSONL parse error: ") + ex.what());
    }
    return Result<MarketEvent>::Ok(ev);
}

Result<std::vector<MarketEvent>> load_events_jsonl(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<std::vector<MarketEvent>>::Err(ErrorCode::IoError, "Cannot open JSONL file: " + path);
    }
    std::vector<MarketEvent> events;
    std::string line;
    size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto parsed = parse_event_json_line(line);
        if (!parsed.ok()) {
            return Result<std::vector<MarketEvent>>::Err(
                parsed.status.code,
                path + ":" + std::to_string(line_no) + ": " + parsed.status.message);
        }
        events.push_back(*parsed.value);
    }
    return Result<std::vector<MarketEvent>>::Ok(std::move(events));
}

Status save_events_jsonl(const std::string& path,
                         const std::vector<MarketEvent>& events,
                         uint16_t schema_version) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::Error(ErrorCode::IoError, "Cannot open JSONL file for writing: " + path);
    }
    for (const auto& ev : events) {
        ofs << event_to_json_line(ev, schema_version) << "\n";
    }
    if (!ofs.good()) {
        return Status::Error(ErrorCode::IoError, "Failed while writing JSONL: " + path);
    }
    return Status::Ok();
}

std::string format_diagnostics_json(const DiagnosticCounters& c) {
    std::ostringstream oss;
    oss << "{"
        << "\"frames_processed\":" << c.frames_processed << ","
        << "\"checkpoint_frames\":" << c.checkpoint_frames << ","
        << "\"delta_frames\":" << c.delta_frames << ","
        << "\"events_processed\":" << c.events_processed << ","
        << "\"sequence_gaps_detected\":" << c.sequence_gaps_detected << ","
        << "\"missing_events_estimated\":" << c.missing_events_estimated << ","
        << "\"crc_errors\":" << c.crc_errors << ","
        << "\"corrupt_frames_skipped\":" << c.corrupt_frames_skipped << ","
        << "\"resync_recoveries\":" << c.resync_recoveries << ","
        << "\"schema_v1_upgrades\":" << c.schema_v1_upgrades << ","
        << "\"bytes_processed\":" << c.bytes_processed
        << "}";
    return oss.str();
}

}  // namespace obce
