// Shared fixtures for the test suite.
#pragma once

#include <functional>
#include <vector>

#include "hftbt/engine.hpp"

namespace hftbt::test {

inline MarketEvent add(Timestamp ts, OrderId id, Side s, Price px, Qty q, InstrumentId i = 0) {
    return {ts, id, px, q, i, MdType::Add, s};
}
inline MarketEvent cancel(Timestamp ts, OrderId id, Qty q, InstrumentId i = 0) {
    return {ts, id, 0, q, i, MdType::Cancel, Side::Buy};
}
inline MarketEvent del(Timestamp ts, OrderId id, InstrumentId i = 0) {
    return {ts, id, 0, 0, i, MdType::Delete, Side::Buy};
}
inline MarketEvent exec(Timestamp ts, OrderId id, Qty q, InstrumentId i = 0) {
    return {ts, id, 0, q, i, MdType::Execute, Side::Buy};
}
inline MarketEvent level(Timestamp ts, Side s, Price px, Qty q, InstrumentId i = 0) {
    return {ts, 0, px, q, i, MdType::Level, s};
}
// `aggressor` is the side of the incoming order.
inline MarketEvent trade(Timestamp ts, Side aggressor, Price px, Qty q, InstrumentId i = 0) {
    return {ts, 0, px, q, i, MdType::Trade, aggressor};
}

inline FeedData feed_of(std::vector<MarketEvent> events, std::size_t instruments = 1) {
    FeedData f;
    for (std::size_t i = 0; i < instruments; ++i) f.symbols.push_back("T" + std::to_string(i));
    f.events = std::move(events);
    return f;
}

struct Recorder final : ExecListener {
    std::vector<ExecReport> reports;
    void on_exec_report(const ExecReport& r) override { reports.push_back(r); }
    std::vector<ExecReport> fills() const {
        std::vector<ExecReport> out;
        for (const ExecReport& r : reports)
            if (r.type == ExecType::Fill) out.push_back(r);
        return out;
    }
    Qty filled(OrderId id) const {
        Qty q = 0;
        for (const ExecReport& r : reports)
            if (r.type == ExecType::Fill && r.order_id == id) q += r.qty;
        return q;
    }
    const ExecReport* last(OrderId id) const {
        for (auto it = reports.rbegin(); it != reports.rend(); ++it)
            if (it->order_id == id) return &*it;
        return nullptr;
    }
};

inline OrderRequest limit(OrderId id, Side s, Price px, Qty q, Tif tif = Tif::GTC,
                          bool post_only = false) {
    OrderRequest r;
    r.id = id;
    r.side = s;
    r.price = px;
    r.qty = q;
    r.tif = tif;
    r.post_only = post_only;
    return r;
}

// Strategy whose behaviour is supplied by the test as lambdas.
struct Script final : Strategy {
    std::function<void(Context&)> start;
    std::function<void(Context&, const MarketEvent&)> md;
    std::function<void(Context&, const ExecReport&)> order;
    std::function<void(Context&, std::uint64_t)> timer;
    struct Seen {
        Timestamp now;
        ExecReport report;
    };
    std::vector<Seen> reports; // every exec report, with the time it was observed

    void on_start(Context& c) override {
        if (start) start(c);
    }
    void on_market_data(Context& c, const MarketEvent& e) override {
        if (md) md(c, e);
    }
    void on_order_update(Context& c, const ExecReport& r) override {
        reports.push_back({c.now(), r});
        if (order) order(c, r);
    }
    void on_timer(Context& c, std::uint64_t id) override {
        if (timer) timer(c, id);
    }
};

} // namespace hftbt::test
