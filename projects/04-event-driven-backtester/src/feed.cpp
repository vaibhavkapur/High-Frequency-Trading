#include "hftbt/feed.hpp"

#include <bit>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace hftbt {

static_assert(std::endian::native == std::endian::little,
              "binary feed format assumes a little-endian host");

namespace {

constexpr char kMagic[8] = {'H', 'F', 'T', 'B', 'T', 'F', 'D', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kRecordSize = 8 + 8 + 8 + 8 + 4 + 1 + 1;
constexpr std::size_t kMaxSymbolLen = 64;
constexpr std::uint32_t kMaxSymbols = 1u << 20;
constexpr Qty kMaxQty = 1'000'000'000'000LL;

template <typename T> bool parse_int(std::string_view s, T& out) {
    if (s.empty()) return false;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

template <typename T> void put(std::vector<std::byte>& buf, const T& v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> d) : d_(d) {}
    std::size_t remaining() const { return d_.size() - pos_; }
    template <typename T> bool get(T& v) {
        if (remaining() < sizeof(T)) return false;
        std::memcpy(&v, d_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool get_bytes(std::size_t n, std::string& out) {
        if (remaining() < n) return false;
        out.assign(reinterpret_cast<const char*>(d_.data() + pos_), n);
        pos_ += n;
        return true;
    }

private:
    std::span<const std::byte> d_;
    std::size_t pos_ = 0;
};

} // namespace

bool validate_feed(const FeedData& feed, std::string& err) {
    Timestamp prev = 0;
    for (std::size_t i = 0; i < feed.events.size(); ++i) {
        const MarketEvent& e = feed.events[i];
        const char* why = nullptr;
        if (e.ts < 0)
            why = "negative timestamp";
        else if (e.ts < prev)
            why = "timestamp goes backwards";
        else if (e.instrument >= feed.symbols.size())
            why = "instrument id out of range";
        else if (!is_valid_md_type(static_cast<std::uint8_t>(e.type)))
            why = "unknown event type";
        else if (static_cast<std::uint8_t>(e.side) > 1)
            why = "invalid side";
        else if (e.qty < 0 || e.qty > kMaxQty)
            why = "qty out of range";
        else if (e.price < 0 || e.price > kMaxPrice)
            why = "price out of range";
        if (why) {
            err = "event " + std::to_string(i) + ": " + why;
            return false;
        }
        prev = e.ts;
    }
    return true;
}

bool parse_csv(std::string_view text, FeedData& out, std::string& err) {
    out = FeedData{};
    std::unordered_map<std::string, InstrumentId> ids;
    std::size_t line_no = 0;
    bool header_seen = false;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++line_no;
        if (line.empty() || line.front() == '#') continue;
        if (!header_seen) {
            header_seen = true;
            if (line.starts_with("ts_ns")) continue;
        }
        std::string_view f[7];
        std::size_t n = 0;
        while (n < 7) {
            const std::size_t c = line.find(',');
            f[n++] = trim(line.substr(0, c));
            if (c == std::string_view::npos) {
                line = {};
                break;
            }
            line.remove_prefix(c + 1);
        }
        const auto fail = [&](const char* why) {
            err = "line " + std::to_string(line_no) + ": " + why;
            return false;
        };
        if (n != 7 || !line.empty()) return fail("expected 7 comma-separated fields");
        MarketEvent e;
        if (!parse_int(f[0], e.ts)) return fail("bad ts_ns");
        if (f[1].empty() || f[1].size() > kMaxSymbolLen) return fail("bad symbol");
        if (f[2].size() != 1 || !is_valid_md_type(static_cast<std::uint8_t>(f[2][0])))
            return fail("bad type (expected one of A,C,D,E,L,T)");
        e.type = static_cast<MdType>(f[2][0]);
        if (!f[3].empty() && !parse_int(f[3], e.order_id)) return fail("bad order_id");
        if (f[4] == "B")
            e.side = Side::Buy;
        else if (f[4] == "S")
            e.side = Side::Sell;
        else if (!f[4].empty())
            return fail("bad side (expected B or S)");
        else if (e.type == MdType::Add || e.type == MdType::Level || e.type == MdType::Trade)
            return fail("side is required for A/L/T events");
        if (!f[5].empty() && !parse_int(f[5], e.price)) return fail("bad price");
        if (!f[6].empty() && !parse_int(f[6], e.qty)) return fail("bad qty");
        const auto [it, inserted] =
            ids.try_emplace(std::string(f[1]), static_cast<InstrumentId>(out.symbols.size()));
        if (inserted) out.symbols.emplace_back(f[1]);
        e.instrument = it->second;
        out.events.push_back(e);
    }
    return validate_feed(out, err);
}

std::string to_csv(const FeedData& feed) {
    std::string s = "ts_ns,symbol,type,order_id,side,price,qty\n";
    s.reserve(s.size() + feed.events.size() * 40);
    for (const MarketEvent& e : feed.events) {
        s += std::to_string(e.ts);
        s += ',';
        s += feed.symbols[e.instrument];
        s += ',';
        s += static_cast<char>(e.type);
        s += ',';
        s += std::to_string(e.order_id);
        s += ',';
        s += e.side == Side::Buy ? 'B' : 'S';
        s += ',';
        s += std::to_string(e.price);
        s += ',';
        s += std::to_string(e.qty);
        s += '\n';
    }
    return s;
}

bool parse_bin(std::span<const std::byte> data, FeedData& out, std::string& err) {
    out = FeedData{};
    Reader rd(data);
    char magic[8];
    std::uint32_t version = 0, n_symbols = 0;
    std::uint64_t n_events = 0;
    if (!rd.get(magic) || std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        err = "bad magic (not an hftbt binary feed)";
        return false;
    }
    if (!rd.get(version) || !rd.get(n_symbols) || !rd.get(n_events)) {
        err = "truncated header";
        return false;
    }
    if (version != kVersion) {
        err = "unsupported version " + std::to_string(version);
        return false;
    }
    if (n_symbols > kMaxSymbols) {
        err = "symbol count out of range";
        return false;
    }
    for (std::uint32_t i = 0; i < n_symbols; ++i) {
        std::uint16_t len = 0;
        std::string sym;
        if (!rd.get(len) || len == 0 || len > kMaxSymbolLen || !rd.get_bytes(len, sym)) {
            err = "bad symbol table";
            return false;
        }
        out.symbols.push_back(std::move(sym));
    }
    if (n_events > rd.remaining() / kRecordSize || n_events * kRecordSize != rd.remaining()) {
        err = "event count does not match payload size";
        return false;
    }
    out.events.resize(static_cast<std::size_t>(n_events));
    for (MarketEvent& e : out.events) {
        std::uint8_t type = 0, side = 0;
        rd.get(e.ts);
        rd.get(e.order_id);
        rd.get(e.price);
        rd.get(e.qty);
        rd.get(e.instrument);
        rd.get(type);
        rd.get(side);
        if (!is_valid_md_type(type) || side > 1) {
            err = "invalid type/side byte in event record";
            out = FeedData{};
            return false;
        }
        e.type = static_cast<MdType>(type);
        e.side = static_cast<Side>(side);
    }
    if (!validate_feed(out, err)) {
        out = FeedData{};
        return false;
    }
    return true;
}

std::vector<std::byte> to_bin(const FeedData& feed) {
    std::vector<std::byte> buf;
    buf.reserve(32 + feed.events.size() * kRecordSize);
    put(buf, kMagic);
    put(buf, kVersion);
    put(buf, static_cast<std::uint32_t>(feed.symbols.size()));
    put(buf, static_cast<std::uint64_t>(feed.events.size()));
    for (const std::string& s : feed.symbols) {
        put(buf, static_cast<std::uint16_t>(s.size()));
        for (char c : s) buf.push_back(static_cast<std::byte>(c));
    }
    for (const MarketEvent& e : feed.events) {
        put(buf, e.ts);
        put(buf, e.order_id);
        put(buf, e.price);
        put(buf, e.qty);
        put(buf, e.instrument);
        put(buf, static_cast<std::uint8_t>(e.type));
        put(buf, static_cast<std::uint8_t>(e.side));
    }
    return buf;
}

bool load_feed(const std::string& path, FeedData& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = std::move(ss).str();
    bool ok;
    if (path.ends_with(".bin"))
        ok = parse_bin(std::as_bytes(std::span(content.data(), content.size())), out, err);
    else
        ok = parse_csv(content, out, err);
    if (!ok) err = path + ": " + err;
    return ok;
}

bool save_feed(const std::string& path, const FeedData& feed, std::string& err) {
    std::ofstream os(path, std::ios::binary);
    if (!os) {
        err = "cannot write " + path;
        return false;
    }
    if (path.ends_with(".bin")) {
        const auto buf = to_bin(feed);
        os.write(reinterpret_cast<const char*>(buf.data()),
                 static_cast<std::streamsize>(buf.size()));
    } else {
        os << to_csv(feed);
    }
    return static_cast<bool>(os);
}

} // namespace hftbt
