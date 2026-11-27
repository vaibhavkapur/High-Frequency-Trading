#include "fpga_ob/contracts.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace fpga_ob {

std::optional<std::string> Config::validate() const {
    if (num_levels < 2 || num_levels > 256 || (num_levels & (num_levels - 1)) != 0) {
        return "num_levels must be a power of 2 in [2, 256]";
    }
    if (max_active_levels == 0 || max_active_levels > num_levels) {
        return "max_active_levels must be in [1, num_levels]";
    }
    if (static_cast<uint64_t>(price_base) + num_levels > std::numeric_limits<uint32_t>::max()) {
        return "price_base + num_levels overflows uint32_t";
    }
    return std::nullopt;
}

namespace {

inline void put_u32_le(std::array<uint8_t, kWireEventSize>& buf, size_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint32_t get_u32_le(const std::array<uint8_t, kWireEventSize>& buf, size_t off) {
    return static_cast<uint32_t>(buf[off + 0]) |
           (static_cast<uint32_t>(buf[off + 1]) << 8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

// Minimal self-contained JSON parser for fixture files (zero external deps)
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, int64_t, std::string, JsonArray, JsonObject> data;

    const JsonObject& as_obj() const { return std::get<JsonObject>(data); }
    const JsonArray& as_arr() const { return std::get<JsonArray>(data); }
    const std::string& as_str() const { return std::get<std::string>(data); }
    int64_t as_i64() const { return std::get<int64_t>(data); }
    bool as_bool() const { return std::get<bool>(data); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) {
            throw std::runtime_error("Trailing characters in JSON at pos " + std::to_string(pos_));
        }
        return v;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    char peek() const {
        if (pos_ >= s_.size()) {
            throw std::runtime_error("Unexpected end of JSON input");
        }
        return s_[pos_];
    }

    void expect(char c) {
        skip_ws();
        if (peek() != c) {
            throw std::runtime_error(std::string("Expected '") + c + "' at pos " + std::to_string(pos_));
        }
        ++pos_;
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JsonValue{parse_string()};
        if (c == 't') {
            match("true");
            return JsonValue{true};
        }
        if (c == 'f') {
            match("false");
            return JsonValue{false};
        }
        if (c == 'n') {
            match("null");
            return JsonValue{nullptr};
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return JsonValue{parse_int()};
        }
        throw std::runtime_error("Invalid JSON token at pos " + std::to_string(pos_));
    }

    void match(const char* kw) {
        for (size_t i = 0; kw[i] != '\0'; ++i) {
            if (pos_ >= s_.size() || s_[pos_] != kw[i]) {
                throw std::runtime_error(std::string("Expected keyword ") + kw);
            }
            ++pos_;
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= s_.size()) throw std::runtime_error("Unterminated escape in JSON string");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("Unterminated JSON string");
    }

    int64_t parse_int() {
        skip_ws();
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        return std::stoll(s_.substr(start, pos_ - start));
    }

    JsonValue parse_array() {
        expect('[');
        JsonArray arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return JsonValue{std::move(arr)};
        }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ']') {
                ++pos_;
                break;
            }
            expect(',');
        }
        return JsonValue{std::move(arr)};
    }

    JsonValue parse_object() {
        expect('{');
        JsonObject obj;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return JsonValue{std::move(obj)};
        }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            expect(':');
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            if (peek() == '}') {
                ++pos_;
                break;
            }
            expect(',');
        }
        return JsonValue{std::move(obj)};
    }

    const std::string& s_;
    size_t pos_ = 0;
};

std::string read_text_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Unable to open file: " + path);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace

std::array<uint8_t, kWireEventSize> encode_event(const EventPacket& pkt) {
    std::array<uint8_t, kWireEventSize> buf{};
    put_u32_le(buf, 0, pkt.seq_no);
    put_u32_le(buf, 4, pkt.timestamp_ns);
    put_u32_le(buf, 8, pkt.price);
    put_u32_le(buf, 12, pkt.qty);
    buf[16] = pkt.opcode;
    buf[17] = pkt.side;
    buf[18] = pkt.flags;
    buf[19] = pkt.schema_ver;
    return buf;
}

EventPacket decode_event(const std::array<uint8_t, kWireEventSize>& bytes) {
    EventPacket pkt{};
    pkt.seq_no       = get_u32_le(bytes, 0);
    pkt.timestamp_ns = get_u32_le(bytes, 4);
    pkt.price        = get_u32_le(bytes, 8);
    pkt.qty          = get_u32_le(bytes, 12);
    pkt.opcode       = bytes[16];
    pkt.side         = bytes[17];
    pkt.flags        = bytes[18];
    pkt.schema_ver   = bytes[19];
    return pkt;
}

const char* status_name(Status st) {
    switch (st) {
        case Status::Ok:                   return "STATUS_OK";
        case Status::ErrUnsupportedSchema: return "ERR_UNSUPPORTED_SCHEMA";
        case Status::ErrUnsupportedFlags:  return "ERR_UNSUPPORTED_FLAGS";
        case Status::ErrUnsupportedOpcode: return "ERR_UNSUPPORTED_OPCODE";
        case Status::ErrInvalidSide:       return "ERR_INVALID_SIDE";
        case Status::ErrPriceOob:          return "ERR_PRICE_OOB";
        case Status::ErrZeroQty:           return "ERR_ZERO_QTY";
        case Status::ErrFullCapacity:      return "ERR_FULL_CAPACITY";
        case Status::ErrOverflowQty:       return "ERR_OVERFLOW_QTY";
        case Status::ErrOverflowOrders:    return "ERR_OVERFLOW_ORDERS";
        case Status::ErrInvalidCancel:     return "ERR_INVALID_CANCEL";
        case Status::ErrInvalidExecute:    return "ERR_INVALID_EXECUTE";
    }
    return "UNKNOWN_STATUS";
}

const char* opcode_name(uint8_t op) {
    switch (op) {
        case 0x00: return "NOP";
        case 0x01: return "ADD";
        case 0x02: return "CANCEL";
        case 0x03: return "EXECUTE";
        default:   return "UNSUPPORTED";
    }
}

const char* side_name(uint8_t side) {
    switch (side) {
        case 0x00: return "BID";
        case 0x01: return "ASK";
        default:   return "INVALID";
    }
}

Status parse_status_name(const std::string& name) {
    if (name == "STATUS_OK") return Status::Ok;
    if (name == "ERR_UNSUPPORTED_SCHEMA") return Status::ErrUnsupportedSchema;
    if (name == "ERR_UNSUPPORTED_FLAGS") return Status::ErrUnsupportedFlags;
    if (name == "ERR_UNSUPPORTED_OPCODE") return Status::ErrUnsupportedOpcode;
    if (name == "ERR_INVALID_SIDE") return Status::ErrInvalidSide;
    if (name == "ERR_PRICE_OOB") return Status::ErrPriceOob;
    if (name == "ERR_ZERO_QTY") return Status::ErrZeroQty;
    if (name == "ERR_FULL_CAPACITY") return Status::ErrFullCapacity;
    if (name == "ERR_OVERFLOW_QTY") return Status::ErrOverflowQty;
    if (name == "ERR_OVERFLOW_ORDERS") return Status::ErrOverflowOrders;
    if (name == "ERR_INVALID_CANCEL") return Status::ErrInvalidCancel;
    if (name == "ERR_INVALID_EXECUTE") return Status::ErrInvalidExecute;
    throw std::runtime_error("Unknown status name: " + name);
}

uint8_t parse_opcode_name(const std::string& name) {
    if (name == "NOP") return static_cast<uint8_t>(Opcode::Nop);
    if (name == "ADD") return static_cast<uint8_t>(Opcode::Add);
    if (name == "CANCEL") return static_cast<uint8_t>(Opcode::Cancel);
    if (name == "EXECUTE") return static_cast<uint8_t>(Opcode::Execute);
    throw std::runtime_error("Unknown opcode name: " + name);
}

uint8_t parse_side_name(const std::string& name) {
    if (name == "BID") return static_cast<uint8_t>(Side::Bid);
    if (name == "ASK") return static_cast<uint8_t>(Side::Ask);
    throw std::runtime_error("Unknown side name: " + name);
}

std::string to_json(const EventPacket& ev) {
    std::ostringstream os;
    os << "{"
       << "\"seq_no\":" << ev.seq_no << ","
       << "\"timestamp_ns\":" << ev.timestamp_ns << ","
       << "\"opcode\":" << static_cast<unsigned>(ev.opcode) << ","
       << "\"opcode_name\":\"" << opcode_name(ev.opcode) << "\","
       << "\"side\":" << static_cast<unsigned>(ev.side) << ","
       << "\"side_name\":\"" << side_name(ev.side) << "\","
       << "\"price\":" << ev.price << ","
       << "\"qty\":" << ev.qty << ","
       << "\"flags\":" << static_cast<unsigned>(ev.flags) << ","
       << "\"schema_ver\":" << static_cast<unsigned>(ev.schema_ver)
       << "}";
    return os.str();
}

std::string to_json(const BboOutputPacket& out) {
    std::ostringstream os;
    os << "{"
       << "\"schema_ver\":" << static_cast<unsigned>(out.schema_ver) << ","
       << "\"lane1_valid\":" << (out.lane1_valid ? "true" : "false") << ","
       << "\"status_0\":\"" << status_name(out.status_0) << "\","
       << "\"status_1\":\"" << status_name(out.status_1) << "\","
       << "\"seq_no_0\":" << out.seq_no_0 << ","
       << "\"seq_no_1\":" << out.seq_no_1 << ","
       << "\"timestamp_ns\":" << out.timestamp_ns << ","
       << "\"best_bid_valid\":" << (out.best_bid_valid ? "true" : "false") << ","
       << "\"best_bid_price\":" << out.best_bid_price << ","
       << "\"best_bid_qty\":" << out.best_bid_qty << ","
       << "\"best_bid_orders\":" << out.best_bid_orders << ","
       << "\"best_ask_valid\":" << (out.best_ask_valid ? "true" : "false") << ","
       << "\"best_ask_price\":" << out.best_ask_price << ","
       << "\"best_ask_qty\":" << out.best_ask_qty << ","
       << "\"best_ask_orders\":" << out.best_ask_orders << ","
       << "\"spread\":" << out.spread << ","
       << "\"crossed\":" << (out.crossed ? "true" : "false") << ","
       << "\"active_bid_levels\":" << out.active_bid_levels << ","
       << "\"active_ask_levels\":" << out.active_ask_levels
       << "}";
    return os.str();
}

FixtureBundle load_events_json(const std::string& path) {
    std::string raw = read_text_file(path);
    JsonValue root = JsonParser(raw).parse();
    const auto& obj = root.as_obj();

    FixtureBundle bundle{};
    if (obj.count("name")) bundle.name = obj.at("name").as_str();
    if (obj.count("schema_version")) bundle.schema_version = obj.at("schema_version").as_str();
    if (obj.count("config")) {
        const auto& c = obj.at("config").as_obj();
        if (c.count("price_base")) bundle.config.price_base = static_cast<uint32_t>(c.at("price_base").as_i64());
        if (c.count("num_levels")) bundle.config.num_levels = static_cast<uint32_t>(c.at("num_levels").as_i64());
        if (c.count("max_active_levels")) bundle.config.max_active_levels = static_cast<uint32_t>(c.at("max_active_levels").as_i64());
    }
    if (auto err = bundle.config.validate(); err.has_value()) {
        throw std::runtime_error("Invalid fixture config: " + *err);
    }

    for (const auto& ev_val : obj.at("events").as_arr()) {
        const auto& e = ev_val.as_obj();
        EventPacket pkt{};
        pkt.seq_no       = static_cast<uint32_t>(e.at("seq_no").as_i64());
        pkt.timestamp_ns = static_cast<uint32_t>(e.at("timestamp_ns").as_i64());
        pkt.price        = static_cast<uint32_t>(e.at("price").as_i64());
        pkt.qty          = static_cast<uint32_t>(e.at("qty").as_i64());
        if (std::holds_alternative<std::string>(e.at("opcode").data)) {
            pkt.opcode = parse_opcode_name(e.at("opcode").as_str());
        } else {
            pkt.opcode = static_cast<uint8_t>(e.at("opcode").as_i64());
        }
        if (std::holds_alternative<std::string>(e.at("side").data)) {
            pkt.side = parse_side_name(e.at("side").as_str());
        } else {
            pkt.side = static_cast<uint8_t>(e.at("side").as_i64());
        }
        pkt.flags      = e.count("flags") ? static_cast<uint8_t>(e.at("flags").as_i64()) : 0;
        pkt.schema_ver = e.count("schema_ver") ? static_cast<uint8_t>(e.at("schema_ver").as_i64()) : kSchemaVersion;
        bundle.events.push_back(pkt);
    }
    return bundle;
}

std::vector<BboOutputPacket> load_expected_json(const std::string& path) {
    std::string raw = read_text_file(path);
    JsonValue root = JsonParser(raw).parse();
    const auto& arr = root.as_obj().at("expected").as_arr();
    std::vector<BboOutputPacket> res;
    res.reserve(arr.size());
    for (const auto& v : arr) {
        const auto& o = v.as_obj();
        BboOutputPacket out{};
        out.schema_ver        = o.count("schema_ver") ? static_cast<uint8_t>(o.at("schema_ver").as_i64()) : kSchemaVersion;
        out.lane1_valid       = o.count("lane1_valid") ? o.at("lane1_valid").as_bool() : false;
        out.status_0          = parse_status_name(o.at("status_0").as_str());
        out.status_1          = o.count("status_1") ? parse_status_name(o.at("status_1").as_str()) : Status::Ok;
        out.seq_no_0          = static_cast<uint32_t>(o.at("seq_no_0").as_i64());
        out.seq_no_1          = o.count("seq_no_1") ? static_cast<uint32_t>(o.at("seq_no_1").as_i64()) : 0;
        out.timestamp_ns      = static_cast<uint32_t>(o.at("timestamp_ns").as_i64());
        out.best_bid_valid    = o.at("best_bid_valid").as_bool();
        out.best_bid_price    = static_cast<uint32_t>(o.at("best_bid_price").as_i64());
        out.best_bid_qty      = static_cast<uint32_t>(o.at("best_bid_qty").as_i64());
        out.best_bid_orders   = static_cast<uint16_t>(o.at("best_bid_orders").as_i64());
        out.best_ask_valid    = o.at("best_ask_valid").as_bool();
        out.best_ask_price    = static_cast<uint32_t>(o.at("best_ask_price").as_i64());
        out.best_ask_qty      = static_cast<uint32_t>(o.at("best_ask_qty").as_i64());
        out.best_ask_orders   = static_cast<uint16_t>(o.at("best_ask_orders").as_i64());
        out.spread            = static_cast<int32_t>(o.at("spread").as_i64());
        out.crossed           = o.at("crossed").as_bool();
        out.active_bid_levels = static_cast<uint16_t>(o.at("active_bid_levels").as_i64());
        out.active_ask_levels = static_cast<uint16_t>(o.at("active_ask_levels").as_i64());
        res.push_back(out);
    }
    return res;
}

void write_binary_stream(const std::string& path, const std::vector<EventPacket>& events) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        throw std::runtime_error("Unable to write binary stream: " + path);
    }
    for (const auto& ev : events) {
        auto frame = encode_event(ev);
        ofs.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    }
}

std::vector<EventPacket> read_binary_stream(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("Unable to read binary stream: " + path);
    }
    std::vector<EventPacket> events;
    std::array<uint8_t, kWireEventSize> frame{};
    while (ifs.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()))) {
        events.push_back(decode_event(frame));
    }
    if (!ifs.eof() && ifs.gcount() != 0) {
        throw std::runtime_error("Truncated 20-byte frame in binary stream: " + path);
    }
    return events;
}

}  // namespace fpga_ob
