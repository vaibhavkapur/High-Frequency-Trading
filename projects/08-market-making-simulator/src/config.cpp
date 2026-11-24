#include "mmsim/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace mmsim {

namespace {

std::string trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !is_space(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !is_space(c); }).base(),
            s.end());
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(const std::string& v, bool& out) {
    const std::string lv = to_lower(v);
    if (lv == "true" || lv == "1" || lv == "yes") {
        out = true;
        return true;
    }
    if (lv == "false" || lv == "0" || lv == "no") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

std::string strategy_type_to_string(StrategyType t) {
    switch (t) {
    case StrategyType::InventoryAware: return "inventory_aware";
    case StrategyType::NaiveSymmetric: return "naive_symmetric";
    }
    return "unknown";
}

std::string fill_model_to_string(FillModelType f) {
    switch (f) {
    case FillModelType::QueuePriority: return "queue_priority";
    case FillModelType::ProbabilisticTouch: return "probabilistic_touch";
    }
    return "unknown";
}

std::string book_impl_to_string(BookImplType b) {
    switch (b) {
    case BookImplType::FlatArray: return "flat_array";
    case BookImplType::StdMap: return "std_map";
    }
    return "unknown";
}

bool apply_config_kv(const std::string& raw_key, const std::string& raw_val, SimConfig& cfg,
                     std::string& err) {
    const std::string key = to_lower(trim(raw_key));
    const std::string val = trim(raw_val);
    try {
        if (key == "tick_size_dollars") {
            cfg.tick_size_dollars = std::stod(val);
            cfg.strategy.tick_size_dollars = cfg.tick_size_dollars;
        } else if (key == "initial_cash_dollars") {
            cfg.initial_cash_dollars = std::stod(val);
        } else if (key == "book_impl") {
            const std::string lv = to_lower(val);
            if (lv == "flat_array" || lv == "flat") {
                cfg.book_impl = BookImplType::FlatArray;
            } else if (lv == "std_map" || lv == "map") {
                cfg.book_impl = BookImplType::StdMap;
            } else {
                err = "Unknown book_impl: " + val;
                return false;
            }
        } else if (key == "md_latency_us") {
            cfg.latency.md_latency_ns = static_cast<TimestampNs>(std::stoll(val)) * kNsPerUs;
        } else if (key == "order_latency_us") {
            cfg.latency.order_latency_ns = static_cast<TimestampNs>(std::stoll(val)) * kNsPerUs;
        } else if (key == "ack_latency_us") {
            cfg.latency.ack_latency_ns = static_cast<TimestampNs>(std::stoll(val)) * kNsPerUs;
        } else if (key == "md_latency_ns") {
            cfg.latency.md_latency_ns = std::stoll(val);
        } else if (key == "order_latency_ns") {
            cfg.latency.order_latency_ns = std::stoll(val);
        } else if (key == "ack_latency_ns") {
            cfg.latency.ack_latency_ns = std::stoll(val);
        } else if (key == "maker_fee_bps") {
            cfg.fees.maker_fee_bps = std::stod(val);
        } else if (key == "taker_fee_bps") {
            cfg.fees.taker_fee_bps = std::stod(val);
        } else if (key == "cancel_fee_dollars") {
            cfg.fees.cancel_fee_dollars = std::stod(val);
        } else if (key == "max_abs_position") {
            cfg.risk.max_abs_position = std::stoll(val);
        } else if (key == "max_order_size") {
            cfg.risk.max_order_size = std::stoll(val);
        } else if (key == "max_gross_notional_dollars") {
            cfg.risk.max_gross_notional_dollars = std::stod(val);
        } else if (key == "max_drawdown_dollars") {
            cfg.risk.max_drawdown_dollars = std::stod(val);
        } else if (key == "max_open_orders") {
            cfg.risk.max_open_orders = std::stoi(val);
        } else if (key == "imbalance_depth") {
            cfg.features.imbalance_depth = std::stoi(val);
        } else if (key == "ofi_halflife_sec") {
            cfg.features.ofi_halflife_sec = std::stod(val);
        } else if (key == "vol_halflife_sec") {
            cfg.features.vol_halflife_sec = std::stod(val);
        } else if (key == "min_vol_ticks_per_sqrt_sec") {
            cfg.features.min_vol_ticks_per_sqrt_sec = std::stod(val);
        } else if (key == "strategy") {
            const std::string lv = to_lower(val);
            if (lv == "inventory_aware") {
                cfg.strategy.type = StrategyType::InventoryAware;
            } else if (lv == "naive_symmetric") {
                cfg.strategy.type = StrategyType::NaiveSymmetric;
            } else {
                err = "Unknown strategy: " + val;
                return false;
            }
        } else if (key == "alpha_obi") {
            cfg.strategy.alpha_obi = std::stod(val);
        } else if (key == "alpha_ofi") {
            cfg.strategy.alpha_ofi = std::stod(val);
        } else if (key == "base_half_spread_ticks") {
            cfg.strategy.base_half_spread_ticks = std::stod(val);
        } else if (key == "min_half_spread_ticks") {
            cfg.strategy.min_half_spread_ticks = std::stod(val);
        } else if (key == "max_half_spread_ticks") {
            cfg.strategy.max_half_spread_ticks = std::stod(val);
        } else if (key == "vol_spread_mult") {
            cfg.strategy.vol_spread_mult = std::stod(val);
        } else if (key == "fee_spread_mult") {
            cfg.strategy.fee_spread_mult = std::stod(val);
        } else if (key == "risk_aversion") {
            cfg.strategy.risk_aversion = std::stod(val);
        } else if (key == "inventory_quad_skew") {
            cfg.strategy.inventory_quad_skew = std::stod(val);
        } else if (key == "base_quote_qty") {
            cfg.strategy.base_quote_qty = std::stoll(val);
        } else if (key == "min_quote_qty") {
            cfg.strategy.min_quote_qty = std::stoll(val);
        } else if (key == "requote_threshold_ticks") {
            cfg.strategy.requote_threshold_ticks = std::stoll(val);
        } else if (key == "min_quote_lifetime_ms") {
            cfg.strategy.min_quote_lifetime_ns = static_cast<TimestampNs>(std::stoll(val)) * kNsPerMs;
        } else if (key == "post_only") {
            if (!parse_bool(val, cfg.strategy.post_only)) {
                err = "Invalid boolean for post_only: " + val;
                return false;
            }
        } else if (key == "fill_model") {
            const std::string lv = to_lower(val);
            if (lv == "queue_priority") {
                cfg.exchange.fill_model = FillModelType::QueuePriority;
            } else if (lv == "probabilistic_touch") {
                cfg.exchange.fill_model = FillModelType::ProbabilisticTouch;
            } else {
                err = "Unknown fill_model: " + val;
                return false;
            }
        } else if (key == "queue_cancel_ahead_fraction") {
            cfg.exchange.queue_cancel_ahead_fraction = std::stod(val);
        } else if (key == "touch_fill_prob") {
            cfg.exchange.touch_fill_prob = std::stod(val);
        } else if (key == "distance_decay_per_tick") {
            cfg.exchange.distance_decay_per_tick = std::stod(val);
        } else if (key == "rng_seed") {
            cfg.exchange.rng_seed = std::stoull(val);
        } else if (key == "equity_sample_interval_ms") {
            cfg.equity_sample_interval_ns = static_cast<TimestampNs>(std::stoll(val)) * kNsPerMs;
        } else {
            err = "Unknown config key: " + key;
            return false;
        }
    } catch (const std::exception& e) {
        err = "Invalid value for " + key + " ('" + val + "'): " + e.what();
        return false;
    }
    return true;
}

bool load_config_file(const std::string& path, SimConfig& out, std::string& err) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        err = "Failed to open config file: " + path;
        return false;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            err = "Malformed line " + std::to_string(line_no) + " in " + path;
            return false;
        }
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (!apply_config_kv(k, v, out, err)) {
            err = "Line " + std::to_string(line_no) + ": " + err;
            return false;
        }
    }
    return true;
}

} // namespace mmsim
