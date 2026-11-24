#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "hftbt/engine.hpp"

namespace hftbt {

namespace {
void appendf(std::string& s, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, static_cast<std::size_t>(std::min<int>(n, sizeof(buf) - 1)));
}
void kv(std::string& s, const char* k, double v) { appendf(s, "  \"%s\": %.10g,\n", k, v); }
void kv(std::string& s, const char* k, std::uint64_t v) {
    appendf(s, "  \"%s\": %llu,\n", k, static_cast<unsigned long long>(v));
}
void kv(std::string& s, const char* k, std::int64_t v) {
    appendf(s, "  \"%s\": %lld,\n", k, static_cast<long long>(v));
}
} // namespace

std::string summary_json(const Summary& s, const std::vector<std::string>& symbols) {
    std::string j = "{\n";
    kv(j, "start_ts", s.start_ts);
    kv(j, "end_ts", s.end_ts);
    kv(j, "feed_events", s.feed_events);
    kv(j, "dispatched_events", s.dispatched_events);
    kv(j, "strategy_callbacks", s.strategy_callbacks);
    kv(j, "data_anomalies", s.data_anomalies);
    kv(j, "orders_sent", s.orders_sent);
    kv(j, "orders_risk_rejected", s.orders_risk_rejected);
    kv(j, "orders_exchange_rejected", s.orders_exchange_rejected);
    kv(j, "orders_fully_filled", s.orders_fully_filled);
    kv(j, "cancels_sent", s.cancels_sent);
    kv(j, "cancel_rejects", s.cancel_rejects);
    kv(j, "fills", s.fills);
    kv(j, "maker_fills", s.maker_fills);
    kv(j, "taker_fills", s.taker_fills);
    kv(j, "qty_submitted", s.qty_submitted);
    kv(j, "qty_filled", s.qty_filled);
    kv(j, "fill_ratio", s.fill_ratio);
    kv(j, "pnl_net", s.pnl_net);
    kv(j, "pnl_gross", s.pnl_gross);
    kv(j, "fees", s.fees);
    kv(j, "realized", s.realized);
    kv(j, "unrealized", s.unrealized);
    kv(j, "turnover", s.turnover);
    kv(j, "max_drawdown", s.max_drawdown);
    kv(j, "avg_gross_exposure", s.avg_gross_exposure);
    kv(j, "max_gross_exposure", s.max_gross_exposure);
    kv(j, "open_orders_at_end", static_cast<std::uint64_t>(s.open_orders_at_end));
    kv(j, "markouts_unresolved", s.markouts_unresolved);
    j += "  \"final_positions\": {";
    for (std::size_t i = 0; i < s.final_positions.size(); ++i)
        appendf(j, "%s\"%s\": %lld", i ? ", " : "", symbols[i].c_str(),
                static_cast<long long>(s.final_positions[i]));
    j += "},\n  \"markouts\": [";
    for (std::size_t i = 0; i < s.markouts.size(); ++i) {
        const MarkoutStats& m = s.markouts[i];
        // Average signed ticks per lot; negative = price moved against the fill.
        const double mk = m.maker_qty ? m.maker_sum / static_cast<double>(m.maker_qty) : 0.0;
        const double tk = m.taker_qty ? m.taker_sum / static_cast<double>(m.taker_qty) : 0.0;
        appendf(j,
                "%s\n    {\"horizon_ns\": %lld, \"maker_avg_ticks\": %.10g, \"maker_qty\": %lld, "
                "\"taker_avg_ticks\": %.10g, \"taker_qty\": %lld}",
                i ? "," : "", static_cast<long long>(m.horizon_ns), mk,
                static_cast<long long>(m.maker_qty), tk, static_cast<long long>(m.taker_qty));
    }
    j += s.markouts.empty() ? "],\n" : "\n  ],\n";
    appendf(j, "  \"event_hash\": \"%016llx\"\n}\n", static_cast<unsigned long long>(s.event_hash));
    return j;
}

bool write_reports(const Engine& engine, const std::string& dir, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    const auto& symbols = engine.feed().symbols;
    {
        std::ofstream os(dir + "/summary.json");
        os << summary_json(engine.summary(), symbols);
        if (!os) {
            err = "cannot write " + dir + "/summary.json";
            return false;
        }
    }
    {
        std::string s = "exch_ts_ns,order_id,symbol,side,liquidity,price,qty,fee\n";
        for (const FillRecord& f : engine.fills())
            appendf(s, "%lld,%llu,%s,%c,%s,%lld,%lld,%.10g\n", static_cast<long long>(f.exch_ts),
                    static_cast<unsigned long long>(f.order_id), symbols[f.instrument].c_str(),
                    f.side == Side::Buy ? 'B' : 'S', f.maker ? "maker" : "taker",
                    static_cast<long long>(f.price), static_cast<long long>(f.qty), f.fee);
        std::ofstream os(dir + "/fills.csv");
        os << s;
        if (!os) {
            err = "cannot write " + dir + "/fills.csv";
            return false;
        }
    }
    {
        std::string s = "ts_ns,equity,gross_exposure,net_position\n";
        for (const EquitySample& e : engine.equity_curve())
            appendf(s, "%lld,%.10g,%.10g,%lld\n", static_cast<long long>(e.ts), e.equity,
                    e.gross_exposure, static_cast<long long>(e.net_position));
        std::ofstream os(dir + "/equity.csv");
        os << s;
        if (!os) {
            err = "cannot write " + dir + "/equity.csv";
            return false;
        }
    }
    return true;
}

} // namespace hftbt
