#include "hftbt/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "hftbt/strategies.hpp"

namespace hftbt {

namespace {
std::string_view trim(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}
} // namespace

bool Config::parse(std::string_view text, std::string& err) {
    std::string section;
    std::size_t line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++line_no;
        if (const std::size_t c = line.find_first_of("#;"); c != std::string_view::npos)
            line = line.substr(0, c);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || trim(line.substr(0, eq)).empty()) {
            err = "config line " + std::to_string(line_no) + ": expected `key = value`";
            return false;
        }
        std::string key(trim(line.substr(0, eq)));
        if (!section.empty()) key = section + "." + key;
        values_[key] = std::string(trim(line.substr(eq + 1)));
    }
    return true;
}

bool Config::load(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open config " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), err);
}

bool Config::set(std::string_view assignment, std::string& err) {
    const std::size_t eq = assignment.find('=');
    if (eq == std::string_view::npos || trim(assignment.substr(0, eq)).empty()) {
        err = "expected section.key=value, got `" + std::string(assignment) + "`";
        return false;
    }
    values_[std::string(trim(assignment.substr(0, eq)))] =
        std::string(trim(assignment.substr(eq + 1)));
    return true;
}

std::string Config::get_str(const std::string& key, const std::string& def) {
    used_.insert(key);
    const auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

std::int64_t Config::get_int(const std::string& key, std::int64_t def) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& s = it->second;
    std::int64_t v = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
        errors_.push_back(key + ": `" + s + "` is not an integer");
        return def;
    }
    return v;
}

double Config::get_double(const std::string& key, double def) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) return def;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (it->second.empty() || *end != '\0') {
        errors_.push_back(key + ": `" + it->second + "` is not a number");
        return def;
    }
    return v;
}

bool Config::get_bool(const std::string& key, bool def) {
    used_.insert(key);
    const auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& s = it->second;
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    errors_.push_back(key + ": `" + s + "` is not a boolean");
    return def;
}

std::vector<std::int64_t> Config::get_int_list(const std::string& key) {
    used_.insert(key);
    std::vector<std::int64_t> out;
    const auto it = values_.find(key);
    if (it == values_.end()) return out;
    std::string_view s = it->second;
    while (!s.empty()) {
        const std::size_t c = s.find(',');
        const std::string_view tok = trim(s.substr(0, c));
        s.remove_prefix(c == std::string_view::npos ? s.size() : c + 1);
        if (tok.empty()) continue;
        std::int64_t v = 0;
        const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v);
        if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size()) {
            errors_.push_back(key + ": `" + std::string(tok) + "` is not an integer");
            continue;
        }
        out.push_back(v);
    }
    return out;
}

std::vector<std::string> Config::problems() const {
    std::vector<std::string> out = errors_;
    for (const auto& [k, v] : values_)
        if (!used_.contains(k)) out.push_back("unknown config key `" + k + "`");
    return out;
}

EngineConfig make_engine_config(Config& c) {
    EngineConfig e;
    e.seed = static_cast<std::uint64_t>(c.get_int("run.seed", 1));
    e.end_ts = c.get_int("run.end_ts", 0);
    e.trade_start_ts = c.get_int("run.trade_start_ts", 0);
    e.sample_interval_ns = c.get_int("run.sample_interval_ns", e.sample_interval_ns);
    e.tick_value = c.get_double("run.tick_value", e.tick_value);
    e.markout_horizons_ns = c.get_int_list("run.markout_horizons_ns");
    e.event_log_path = c.get_str("run.event_log");
    const std::string mdq = c.get_str("run.md_queue", "fifo");
    e.md_fifo = mdq != "heap";

    e.latency.md_delay_ns = c.get_int("latency.md_delay_ns", 0);
    e.latency.md_jitter_ns = c.get_int("latency.md_jitter_ns", 0);
    e.latency.compute_delay_ns = c.get_int("latency.compute_delay_ns", 0);
    e.latency.order_delay_ns = c.get_int("latency.order_delay_ns", 0);
    e.latency.order_jitter_ns = c.get_int("latency.order_jitter_ns", 0);
    e.latency.response_delay_ns = c.get_int("latency.response_delay_ns", 0);
    e.latency.response_jitter_ns = c.get_int("latency.response_jitter_ns", 0);

    const std::string qm = c.get_str("exchange.queue_model", "conservative");
    e.exchange.queue_model = qm == "optimistic"     ? QueueModel::Optimistic
                             : qm == "proportional" ? QueueModel::Proportional
                                                    : QueueModel::Conservative;
    e.exchange.fill_on_crossing_quote = c.get_bool("exchange.fill_on_crossing_quote", true);
    e.exchange.taker_slippage_ticks = c.get_int("exchange.taker_slippage_ticks", 0);

    e.fees.maker_bps = c.get_double("fees.maker_bps", 0.0);
    e.fees.taker_bps = c.get_double("fees.taker_bps", 0.0);

    e.risk.max_order_qty = c.get_int("risk.max_order_qty", e.risk.max_order_qty);
    e.risk.max_position = c.get_int("risk.max_position", e.risk.max_position);
    e.risk.max_open_orders = static_cast<std::size_t>(
        c.get_int("risk.max_open_orders", static_cast<std::int64_t>(e.risk.max_open_orders)));
    return e;
}

std::unique_ptr<Strategy> make_strategy(Config& c, std::string& err) {
    const std::string name = c.get_str("strategy.name", "noop");
    if (name == "noop") return std::make_unique<NoopStrategy>();
    if (name == "market_maker") {
        MarketMakerParams p;
        p.half_spread_ticks = c.get_int("strategy.half_spread_ticks", p.half_spread_ticks);
        p.order_qty = c.get_int("strategy.order_qty", p.order_qty);
        p.max_position = c.get_int("strategy.max_position", p.max_position);
        p.skew_ticks_per_lot = c.get_double("strategy.skew_ticks_per_lot", p.skew_ticks_per_lot);
        p.requote_threshold_ticks =
            c.get_int("strategy.requote_threshold_ticks", p.requote_threshold_ticks);
        p.min_requote_interval_ns =
            c.get_int("strategy.min_requote_interval_ns", p.min_requote_interval_ns);
        p.post_only = c.get_bool("strategy.post_only", p.post_only);
        return std::make_unique<MarketMaker>(p);
    }
    if (name == "imbalance") {
        ImbalanceParams p;
        p.threshold = c.get_double("strategy.threshold", p.threshold);
        p.order_qty = c.get_int("strategy.order_qty", p.order_qty);
        p.max_position = c.get_int("strategy.max_position", p.max_position);
        p.cooldown_ns = c.get_int("strategy.cooldown_ns", p.cooldown_ns);
        p.max_hold_ns = c.get_int("strategy.max_hold_ns", p.max_hold_ns);
        return std::make_unique<ImbalanceTaker>(p);
    }
    if (name == "ladder") {
        LadderParams p;
        p.levels = static_cast<std::uint32_t>(c.get_int("strategy.levels", p.levels));
        p.order_qty = c.get_int("strategy.order_qty", p.order_qty);
        p.offset_ticks = c.get_int("strategy.offset_ticks", p.offset_ticks);
        return std::make_unique<Ladder>(p);
    }
    err = "unknown strategy `" + name + "` (expected noop, market_maker, imbalance or ladder)";
    return nullptr;
}

} // namespace hftbt
