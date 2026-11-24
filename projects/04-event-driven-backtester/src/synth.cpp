#include "hftbt/synth.hpp"

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>

#include "hftbt/order_book.hpp"
#include "hftbt/rng.hpp"

namespace hftbt {

namespace {

struct GOrder {
    Price price;
    Qty qty;
    Side side;
    std::size_t live_idx;
};

struct GLevel {
    std::deque<OrderId> queue; // may contain ids that were cancelled (lazy removal)
    int alive = 0;
};

struct GBook {
    std::map<Price, GLevel> levels[2];
    std::unordered_map<OrderId, GOrder> orders;
    std::vector<OrderId> live[2];

    std::optional<Price> best(Side s) const {
        const auto& m = levels[static_cast<int>(s)];
        if (m.empty()) return std::nullopt;
        return s == Side::Buy ? m.rbegin()->first : m.begin()->first;
    }

    void insert(OrderId id, Side s, Price p, Qty q) {
        auto& lv = live[static_cast<int>(s)];
        orders.emplace(id, GOrder{p, q, s, lv.size()});
        lv.push_back(id);
        GLevel& l = levels[static_cast<int>(s)][p];
        l.queue.push_back(id);
        ++l.alive;
    }

    void remove(OrderId id) {
        const auto it = orders.find(id);
        const GOrder o = it->second;
        auto& lv = live[static_cast<int>(o.side)];
        lv[o.live_idx] = lv.back();
        orders[lv.back()].live_idx = o.live_idx;
        lv.pop_back();
        orders.erase(id);
        auto& m = levels[static_cast<int>(o.side)];
        const auto l = m.find(o.price);
        if (--l->second.alive == 0) m.erase(l);
    }

    // Oldest live order at the best price of side s.
    OrderId front(Side s) {
        auto& m = levels[static_cast<int>(s)];
        GLevel& l = s == Side::Buy ? m.rbegin()->second : m.begin()->second;
        while (!orders.contains(l.queue.front())) l.queue.pop_front();
        return l.queue.front();
    }
};

constexpr std::uint64_t kTargetOrders = 60; // resting orders per instrument at equilibrium

} // namespace

FeedData generate_l3(const SynthConfig& cfg) {
    FeedData feed;
    const std::uint32_t n = std::max<std::uint32_t>(1, cfg.instruments);
    for (std::uint32_t i = 0; i < n; ++i) feed.symbols.push_back("SYN" + std::to_string(i));
    feed.events.reserve(cfg.events);

    Rng rng(cfg.seed);
    std::vector<GBook> books(n);
    OrderId next_id = 1;
    Timestamp ts = cfg.start_ts;
    const Timestamp max_gap = std::max<Timestamp>(1, 2 * cfg.mean_gap_ns);
    const auto full = [&] { return feed.events.size() >= cfg.events; };

    const auto add = [&](InstrumentId i, Side s) {
        GBook& b = books[i];
        const int dir = sign(s); // +1: higher is more aggressive
        const auto own = b.best(s);
        const auto opp = b.best(opposite(s));
        Price px;
        if (opp) {
            // Anchor to the OPPOSITE touch: depth is spread over the eight
            // ticks behind it, so a swept side refills and the spread closes.
            px = *opp - dir * rng.range(1, 8);
        } else if (own) {
            px = *own - dir * rng.range(0, 3);
        } else {
            px = cfg.initial_mid - dir * rng.range(1, 3);
        }
        if (opp && !better(s, *opp, px)) px = *opp - dir; // never cross or lock
        if (px < 1) px = 1;
        if (opp && !better(s, *opp, px)) return; // no room below the ask at px == 1
        const Qty qty = rng.range(1, 100);
        const OrderId id = next_id++;
        b.insert(id, s, px, qty);
        feed.events.push_back({ts, id, px, qty, i, MdType::Add, s});
    };

    for (InstrumentId i = 0; i < n && !full(); ++i)
        for (int k = 0; k < 40 && !full(); ++k) {
            add(i, k % 2 == 0 ? Side::Buy : Side::Sell);
            ts += 1;
        }

    while (!full()) {
        const auto i = static_cast<InstrumentId>(rng.below(n));
        GBook& b = books[i];
        ts += rng.range(1, max_gap);
        const std::uint64_t roll = rng.below(100);
        // Birth-death balance: the fuller the book, the likelier a cancel.
        // Keeps depth stationary so trades regularly clear a level and the
        // price actually moves.
        const std::uint64_t resting = b.live[0].size() + b.live[1].size();
        const bool want_add = rng.below(resting + kTargetOrders) < kTargetOrders;
        if (b.live[0].size() < 5) {
            add(i, Side::Buy);
        } else if (b.live[1].size() < 5) {
            add(i, Side::Sell);
        } else if (roll < 92 && want_add) {
            add(i, rng.chance(50) ? Side::Buy : Side::Sell);
        } else if (roll < 92) {
            const auto& lv = b.live[rng.below(2)];
            const OrderId id = lv[rng.below(lv.size())];
            const GOrder& o = b.orders[id];
            if (o.qty > 1 && rng.chance(30)) {
                const Qty cut = rng.range(1, o.qty - 1);
                feed.events.push_back({ts, id, o.price, cut, i, MdType::Cancel, o.side});
                b.orders[id].qty -= cut;
            } else {
                feed.events.push_back({ts, id, o.price, o.qty, i, MdType::Delete, o.side});
                b.remove(id);
            }
        } else {
            // Aggressor sweeps up to six resting orders at one timestamp.
            Side hit = rng.chance(50) ? Side::Buy : Side::Sell;
            const Price ref = *b.best(Side::Buy);
            if (ref < cfg.initial_mid / 2) hit = Side::Sell;    // keep prices positive
            if (ref > cfg.initial_mid * 3 / 2) hit = Side::Buy; // and bounded
            Qty rem = rng.range(1, 250);
            for (int k = 0; k < 6 && rem > 0 && !full() && b.live[static_cast<int>(hit)].size() > 1;
                 ++k) {
                const OrderId id = b.front(hit);
                GOrder& o = b.orders[id];
                const Qty q = std::min(rem, o.qty);
                feed.events.push_back({ts, id, o.price, q, i, MdType::Execute, hit});
                rem -= q;
                o.qty -= q;
                if (o.qty == 0) b.remove(id);
            }
        }
    }
    return feed;
}

FeedData l3_to_l2(const FeedData& l3) {
    FeedData out;
    out.symbols = l3.symbols;
    out.events.reserve(l3.events.size());
    std::vector<OrderBook> books(l3.symbols.size());
    for (const MarketEvent& e : l3.events) {
        if (e.type == MdType::Level || e.type == MdType::Trade) {
            out.events.push_back(e);
            continue;
        }
        const ApplyResult r = books[e.instrument].apply(e);
        if (!r.ok) continue;
        if (e.type == MdType::Execute)
            out.events.push_back(
                {e.ts, 0, r.price, r.qty, e.instrument, MdType::Trade, opposite(r.side)});
        out.events.push_back({e.ts, 0, r.price, r.after, e.instrument, MdType::Level, r.side});
    }
    return out;
}

} // namespace hftbt
