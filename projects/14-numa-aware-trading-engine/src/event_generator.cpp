#include "numa_engine/event_generator.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace numa_engine {

namespace {

// SplitMix64: simple, fast, 100% deterministic across compilers and platforms.
struct SplitMix64 {
    std::uint64_t state;

    explicit SplitMix64(std::uint64_t seed) noexcept : state(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string to_upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s[0] == '-') return false;
    try {
        std::size_t pos = 0;
        out = std::stoull(s, &pos, 10);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_i64(const std::string& s, std::int64_t& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        out = std::stoll(s, &pos, 10);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_event_type(const std::string& raw, std::uint8_t& out) {
    const std::string u = to_upper(raw);
    if (u == "ADD" || u == "1") { out = static_cast<std::uint8_t>(EventType::kAdd); return true; }
    if (u == "MODIFY" || u == "2") { out = static_cast<std::uint8_t>(EventType::kModify); return true; }
    if (u == "CANCEL" || u == "3") { out = static_cast<std::uint8_t>(EventType::kCancel); return true; }
    if (u == "TRADE" || u == "4") { out = static_cast<std::uint8_t>(EventType::kTrade); return true; }
    return false;
}

bool parse_side(const std::string& raw, std::uint8_t& out) {
    const std::string u = to_upper(raw);
    if (u == "BID" || u == "BUY" || u == "1") { out = static_cast<std::uint8_t>(Side::kBid); return true; }
    if (u == "ASK" || u == "SELL" || u == "2") { out = static_cast<std::uint8_t>(Side::kAsk); return true; }
    return false;
}

}  // namespace

Error validate_event(
    const MarketEvent& ev,
    std::uint64_t previous_seq_id,
    std::size_t line_number) {
    if (ev.seq_id == 0 || ev.seq_id <= previous_seq_id) {
        return Error{
            ErrorCode::kNonMonotonicSequence,
            "seq_id must be strictly greater than previous seq_id (" +
                std::to_string(previous_seq_id) + "), got " + std::to_string(ev.seq_id),
            "seq_id",
            line_number};
    }

    if (ev.symbol_id >= kMaxSymbols) {
        return Error{
            ErrorCode::kInvalidSymbol,
            "symbol_id " + std::to_string(ev.symbol_id) + " exceeds kMaxSymbols (" +
                std::to_string(kMaxSymbols) + ")",
            "symbol_id",
            line_number};
    }

    if (ev.event_type < static_cast<std::uint8_t>(EventType::kAdd) ||
        ev.event_type > static_cast<std::uint8_t>(EventType::kTrade)) {
        return Error{
            ErrorCode::kInvalidEventType,
            "Unknown event_type value: " + std::to_string(ev.event_type),
            "event_type",
            line_number};
    }

    if (ev.side != static_cast<std::uint8_t>(Side::kBid) &&
        ev.side != static_cast<std::uint8_t>(Side::kAsk)) {
        return Error{
            ErrorCode::kInvalidSide,
            "Unknown side value: " + std::to_string(ev.side),
            "side",
            line_number};
    }

    const auto type = static_cast<EventType>(ev.event_type);
    if (type == EventType::kCancel) {
        if (ev.price_ticks < 0) {
            return Error{
                ErrorCode::kInvalidPrice,
                "price_ticks must be >= 0 for CANCEL events, got " + std::to_string(ev.price_ticks),
                "price_ticks",
                line_number};
        }
    } else {
        if (ev.price_ticks <= 0) {
            return Error{
                ErrorCode::kInvalidPrice,
                "price_ticks must be > 0 for ADD/MODIFY/TRADE events, got " + std::to_string(ev.price_ticks),
                "price_ticks",
                line_number};
        }
        if (ev.quantity == 0) {
            return Error{
                ErrorCode::kInvalidQuantity,
                "quantity must be > 0 for ADD/MODIFY/TRADE events",
                "quantity",
                line_number};
        }
    }

    return Error{};
}

Error validate_event_stream(const std::vector<MarketEvent>& events) {
    if (events.empty()) {
        return Error{
            ErrorCode::kInvalidEventSchema,
            "Event stream must contain at least one event",
            "events",
            0};
    }
    std::uint64_t prev_seq = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Error err = validate_event(events[i], prev_seq, i + 1);
        if (!err.ok()) return err;
        prev_seq = events[i].seq_id;
    }
    return Error{};
}

Error validate_generator_config(const GeneratorConfig& cfg) {
    if (cfg.num_events == 0) {
        return Error{ErrorCode::kInvalidConfig, "num_events must be > 0", "num_events", 0};
    }
    if (cfg.num_symbols == 0 || cfg.num_symbols > kMaxSymbols) {
        return Error{
            ErrorCode::kInvalidSymbol,
            "num_symbols must be in [1, " + std::to_string(kMaxSymbols) + "]",
            "num_symbols",
            0};
    }
    if (cfg.base_price_ticks <= 1000) {
        return Error{
            ErrorCode::kInvalidPrice,
            "base_price_ticks must be > 1000",
            "base_price_ticks",
            0};
    }
    return Error{};
}

Result<std::vector<MarketEvent>> generate_synthetic_events(const GeneratorConfig& cfg) {
    const Error err = validate_generator_config(cfg);
    if (!err.ok()) return err;

    const std::size_t total = cfg.warmup_events + cfg.num_events;
    std::vector<MarketEvent> events;
    events.reserve(total);

    SplitMix64 rng(cfg.seed);
    std::vector<std::int64_t> mid_prices(cfg.num_symbols, cfg.base_price_ticks);
    for (std::uint16_t s = 0; s < cfg.num_symbols; ++s) {
        mid_prices[s] = cfg.base_price_ticks + static_cast<std::int64_t>(s) * 25'000;
    }

    for (std::size_t i = 0; i < total; ++i) {
        const std::uint64_t r1 = rng.next();
        const std::uint64_t r2 = rng.next();

        const std::uint16_t sym = static_cast<std::uint16_t>(r1 % cfg.num_symbols);
        const std::uint64_t bucket = (r1 >> 16) % 100;

        EventType type = EventType::kAdd;
        if (bucket < 45) {
            type = EventType::kAdd;
        } else if (bucket < 65) {
            type = EventType::kModify;
        } else if (bucket < 80) {
            type = EventType::kCancel;
        } else {
            type = EventType::kTrade;
        }

        const Side side = ((r1 >> 24) & 1U) == 0 ? Side::kBid : Side::kAsk;

        // Deterministic mean-reverting random walk in integer ticks
        const std::int64_t step = static_cast<std::int64_t>(r2 % 11) - 5;  // [-5, +5] ticks
        mid_prices[sym] = std::max<std::int64_t>(1000, mid_prices[sym] + step);

        const std::int64_t spread_offset = static_cast<std::int64_t>((r2 >> 8) % 5) + 1;
        const std::int64_t price = (side == Side::kBid)
            ? std::max<std::int64_t>(1, mid_prices[sym] - spread_offset)
            : (mid_prices[sym] + spread_offset);

        const std::uint32_t qty = static_cast<std::uint32_t>(((r2 >> 16) % 500) + 1);

        MarketEvent ev{};
        ev.seq_id = static_cast<std::uint64_t>(i + 1);
        ev.send_ts_ns = 0;  // Populated by producer on live send
        ev.price_ticks = price;
        ev.quantity = qty;
        ev.symbol_id = sym;
        ev.event_type = static_cast<std::uint8_t>(type);
        ev.side = static_cast<std::uint8_t>(side);
        ev.order_id = 1'000'000ULL + static_cast<std::uint64_t>(i + 1);
        ev.flags = (i < cfg.warmup_events) ? kEventFlagWarmup : 0U;
        events.push_back(ev);
    }

    return events;
}

Result<std::vector<MarketEvent>> load_events_csv(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Error{ErrorCode::kFileNotFound, "Could not open event fixture: " + path, "path", 0};
    }

    std::vector<MarketEvent> events;
    std::string line;
    std::size_t line_no = 0;
    std::uint64_t prev_seq = 0;
    bool header_seen = false;

    while (std::getline(ifs, line)) {
        ++line_no;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (!header_seen) {
            header_seen = true;
            if (trimmed.rfind("seq_id,", 0) == 0) {
                continue;
            }
            return Error{
                ErrorCode::kInvalidEventSchema,
                "Missing expected CSV header 'seq_id,symbol_id,event_type,side,price_ticks,quantity,order_id,flags'",
                "header",
                line_no};
        }

        std::vector<std::string> cols;
        std::stringstream ss(trimmed);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            cols.push_back(trim(cell));
        }

        if (cols.size() != 8) {
            return Error{
                ErrorCode::kParseError,
                "Expected 8 CSV columns, found " + std::to_string(cols.size()),
                "columns",
                line_no};
        }

        std::uint64_t seq = 0, sym = 0, qty = 0, oid = 0, flags = 0;
        std::int64_t price = 0;
        std::uint8_t ev_type = 0, side = 0;

        if (!parse_u64(cols[0], seq)) {
            return Error{ErrorCode::kParseError, "Invalid seq_id: " + cols[0], "seq_id", line_no};
        }
        if (!parse_u64(cols[1], sym) || sym > 65535ULL) {
            return Error{ErrorCode::kInvalidSymbol, "Invalid symbol_id: " + cols[1], "symbol_id", line_no};
        }
        if (!parse_event_type(cols[2], ev_type)) {
            return Error{ErrorCode::kInvalidEventType, "Invalid event_type: " + cols[2], "event_type", line_no};
        }
        if (!parse_side(cols[3], side)) {
            return Error{ErrorCode::kInvalidSide, "Invalid side: " + cols[3], "side", line_no};
        }
        if (!parse_i64(cols[4], price)) {
            return Error{ErrorCode::kParseError, "Invalid price_ticks: " + cols[4], "price_ticks", line_no};
        }
        if (!parse_u64(cols[5], qty) || qty > 0xFFFFFFFFULL) {
            return Error{ErrorCode::kParseError, "Invalid quantity: " + cols[5], "quantity", line_no};
        }
        if (!parse_u64(cols[6], oid)) {
            return Error{ErrorCode::kParseError, "Invalid order_id: " + cols[6], "order_id", line_no};
        }
        if (!parse_u64(cols[7], flags) || flags > 0xFFFFFFFFULL) {
            return Error{ErrorCode::kParseError, "Invalid flags: " + cols[7], "flags", line_no};
        }

        MarketEvent ev{};
        ev.seq_id = seq;
        ev.symbol_id = static_cast<std::uint16_t>(sym);
        ev.event_type = ev_type;
        ev.side = side;
        ev.price_ticks = price;
        ev.quantity = static_cast<std::uint32_t>(qty);
        ev.order_id = oid;
        ev.flags = static_cast<std::uint32_t>(flags);

        const Error verr = validate_event(ev, prev_seq, line_no);
        if (!verr.ok()) return verr;
        prev_seq = ev.seq_id;
        events.push_back(ev);
    }

    if (events.empty()) {
        return Error{
            ErrorCode::kInvalidEventSchema,
            "Fixture contains no event rows: " + path,
            "events",
            line_no};
    }

    return events;
}

Error save_events_csv(const std::string& path, const std::vector<MarketEvent>& events) {
    const Error verr = validate_event_stream(events);
    if (!verr.ok()) return verr;

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        return Error{ErrorCode::kFileNotFound, "Could not open output file: " + path, "path", 0};
    }

    ofs << "seq_id,symbol_id,event_type,side,price_ticks,quantity,order_id,flags\n";
    for (const auto& ev : events) {
        ofs << ev.seq_id << ','
            << ev.symbol_id << ','
            << to_string(static_cast<EventType>(ev.event_type)) << ','
            << to_string(static_cast<Side>(ev.side)) << ','
            << ev.price_ticks << ','
            << ev.quantity << ','
            << ev.order_id << ','
            << ev.flags << '\n';
    }
    return Error{};
}

}  // namespace numa_engine
