#include "hftbt/exchange.hpp"

#include <algorithm>

namespace hftbt {

const char* to_string(ExecType t) noexcept {
    switch (t) {
    case ExecType::Ack: return "ACK";
    case ExecType::Fill: return "FILL";
    case ExecType::Cancelled: return "CANCELLED";
    case ExecType::Rejected: return "REJECTED";
    case ExecType::CancelRejected: return "CANCEL_REJECTED";
    }
    return "?";
}

const char* to_string(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::InvalidOrder: return "invalid_order";
    case RejectReason::PostOnlyWouldCross: return "post_only_would_cross";
    case RejectReason::TooLateToCancel: return "too_late_to_cancel";
    case RejectReason::RiskMaxOrderQty: return "risk_max_order_qty";
    case RejectReason::RiskMaxPosition: return "risk_max_position";
    case RejectReason::RiskMaxOpenOrders: return "risk_max_open_orders";
    }
    return "?";
}

Exchange::Exchange(std::size_t n_instruments, const ExchangeConfig& cfg, ExecListener& listener)
    : cfg_(cfg), listener_(listener), inst_(n_instruments) {}

std::uint64_t Exchange::anomalies() const noexcept {
    std::uint64_t n = 0;
    for (const Instrument& i : inst_) n += i.book.anomalies();
    return n;
}

const SimOrder* Exchange::find(OrderId id) const {
    const auto it = live_.find(id);
    if (it == live_.end()) return nullptr;
    const Instrument& st = inst_[it->second.first];
    const auto& v = it->second.second == Side::Buy ? st.bids : st.asks;
    for (const SimOrder& o : v)
        if (o.id == id) return &o;
    return nullptr;
}

void Exchange::report(ExecType type, const OrderRequest& req, Timestamp now, RejectReason why,
                      Qty qty, Qty leaves) {
    ExecReport r;
    r.exch_ts = now;
    r.order_id = req.id;
    r.instrument = req.instrument;
    r.type = type;
    r.side = req.side;
    r.reason = why;
    r.qty = qty;
    r.leaves = leaves;
    listener_.on_exec_report(r);
}

void Exchange::on_market_event(const MarketEvent& e) {
    if (e.instrument >= inst_.size()) return;
    Instrument& st = inst_[e.instrument];
    const ApplyResult r = st.book.apply(e);
    if (!r.ok || (st.bids.empty() && st.asks.empty())) return;

    switch (e.type) {
    case MdType::Add:
        if (cfg_.fill_on_crossing_quote)
            match_passive(st, e.instrument, opposite(r.side), r.price, r.qty, 0,
                          Passive::CrossingQuote, e.ts);
        break;
    case MdType::Cancel:
    case MdType::Delete: reduce_ahead_l3(st, r.side, r.price, r.qty, r.seq); break;
    case MdType::Execute:
        match_passive(st, e.instrument, r.side, r.price, r.qty, r.seq, Passive::L3Execute, e.ts);
        break;
    case MdType::Trade:
        match_passive(st, e.instrument, r.side, r.price, r.qty, 0, Passive::L2Trade, e.ts);
        break;
    case MdType::Level:
        if (r.after < r.before) {
            const Qty dec = r.before - r.after;
            const Qty from_trades = std::min(dec, r.pending_trade);
            reduce_ahead_l2(st, r.side, r.price, r.before - from_trades, dec - from_trades,
                            r.after);
        } else if (r.after > r.before && cfg_.fill_on_crossing_quote) {
            match_passive(st, e.instrument, opposite(r.side), r.price, r.after - r.before, 0,
                          Passive::CrossingQuote, e.ts);
        }
        break;
    }
}

// A historical trade / crossing quote of size `qty` at `px` interacts with our
// resting orders on `our_side`. Orders priced through `px` are filled first;
// at `px` itself the historical queue ahead of us trades before we do.
void Exchange::match_passive(Instrument& st, InstrumentId instr, Side our_side, Price px, Qty qty,
                             std::uint64_t seq, Passive kind, Timestamp now) {
    auto& orders = st.orders(our_side);
    Qty own_filled = 0; // qty our earlier (higher-priority) orders already took
    bool any_done = false;
    for (SimOrder& o : orders) {
        if (better(our_side, px, o.price)) break; // o is priced worse than the trade
        Qty eligible = qty;
        if (o.price == px && kind != Passive::CrossingQuote) {
            if (kind == Passive::L3Execute) {
                if (seq < o.join_seq) { // the executed order was ahead of us
                    o.ahead = std::max<Qty>(0, o.ahead - qty);
                    continue;
                }
            } else {
                eligible = qty - o.ahead;
                o.ahead = std::max<Qty>(0, o.ahead - qty);
            }
        }
        const Qty fill = std::min(o.remaining, eligible - own_filled);
        if (fill <= 0) continue;
        o.remaining -= fill;
        own_filled += fill;
        ExecReport r;
        r.exch_ts = now;
        r.order_id = o.id;
        r.instrument = instr;
        r.type = ExecType::Fill;
        r.side = our_side;
        r.maker = true;
        r.price = o.price;
        r.qty = fill;
        r.leaves = o.remaining;
        if (o.remaining == 0) {
            any_done = true;
            live_.erase(o.id);
        }
        listener_.on_exec_report(r);
    }
    if (any_done) std::erase_if(orders, [](const SimOrder& o) { return o.remaining == 0; });
}

void Exchange::reduce_ahead_l3(Instrument& st, Side s, Price px, Qty removed, std::uint64_t seq) {
    for (SimOrder& o : st.orders(s)) {
        if (better(s, px, o.price)) break;
        if (o.price == px && seq < o.join_seq) o.ahead = std::max<Qty>(0, o.ahead - removed);
    }
}

// `base`: level qty before the cancellation (trades already removed).
void Exchange::reduce_ahead_l2(Instrument& st, Side s, Price px, Qty base, Qty cancelled,
                               Qty after) {
    for (SimOrder& o : st.orders(s)) {
        if (better(s, px, o.price)) break;
        if (o.price != px) continue;
        if (cancelled > 0) {
            switch (cfg_.queue_model) {
            case QueueModel::Conservative: break; // only the clamp below applies
            case QueueModel::Optimistic: o.ahead -= cancelled; break;
            case QueueModel::Proportional:
                // Rounded down: never credit a fractional lot of queue progress.
                if (base > 0) o.ahead -= cancelled * std::min(o.ahead, base) / base;
                break;
            }
        }
        // Whatever the model, no more can be ahead of us than is displayed.
        o.ahead = std::clamp<Qty>(o.ahead, 0, after);
    }
}

void Exchange::on_new_order(const OrderRequest& req, Timestamp now) {
    const bool market = req.type == OrdType::Market;
    if (req.instrument >= inst_.size() || req.qty <= 0 || (!market && req.price <= 0) ||
        req.price > kMaxPrice || (market && req.post_only) || live_.contains(req.id)) {
        report(ExecType::Rejected, req, now, RejectReason::InvalidOrder, req.qty, 0);
        return;
    }
    Instrument& st = inst_[req.instrument];
    BookSide& opp = st.book.side_mut(opposite(req.side));
    const Price limit = market ? (req.side == Side::Buy ? kMaxPrice : 0) : req.price;
    const auto crosses = [&](Price level_px) { return !better(req.side, level_px, limit); };

    if (req.post_only && opp.best() && crosses(opp.best()->price)) {
        report(ExecType::Rejected, req, now, RejectReason::PostOnlyWouldCross, req.qty, 0);
        return;
    }
    report(ExecType::Ack, req, now, RejectReason::None, 0, req.qty);

    // Aggressive part: walk displayed liquidity. `consumed` stops us from
    // taking the same displayed lots twice before the level next changes.
    Qty rem = req.qty;
    for (std::size_t i = 0; i < opp.size() && rem > 0; ++i) {
        Level& lvl = opp.from_best(i);
        if (!crosses(lvl.price)) break;
        const Qty take = std::min(rem, lvl.qty - lvl.consumed);
        if (take <= 0) continue;
        lvl.consumed += take;
        rem -= take;
        ExecReport r;
        r.exch_ts = now;
        r.order_id = req.id;
        r.instrument = req.instrument;
        r.type = ExecType::Fill;
        r.side = req.side;
        r.maker = false;
        r.price = std::max<Price>(1, lvl.price + sign(req.side) * cfg_.taker_slippage_ticks);
        r.qty = take;
        r.leaves = rem;
        listener_.on_exec_report(r);
    }
    if (rem == 0) return;
    if (market || req.tif == Tif::IOC) {
        report(ExecType::Cancelled, req, now, RejectReason::None, rem, 0);
        return;
    }

    // Passive remainder joins the back of the queue at its price.
    SimOrder o;
    o.id = req.id;
    o.side = req.side;
    o.price = req.price;
    o.qty = req.qty;
    o.remaining = rem;
    o.ahead = st.book.side(req.side).qty_at(req.price);
    o.join_seq = st.book.next_seq();
    auto& orders = st.orders(req.side);
    const auto pos = std::find_if(orders.begin(), orders.end(), [&](const SimOrder& x) {
        return better(req.side, req.price, x.price);
    });
    orders.insert(pos, o);
    live_.emplace(req.id, std::make_pair(req.instrument, req.side));
}

void Exchange::on_cancel(const CancelRequest& req, Timestamp now) {
    OrderRequest ident;
    ident.id = req.id;
    ident.instrument = req.instrument;
    const auto it = live_.find(req.id);
    if (it == live_.end()) {
        // Race lost: the order filled (or was never accepted) before the
        // cancel arrived. Fills already reported stand.
        report(ExecType::CancelRejected, ident, now, RejectReason::TooLateToCancel, 0, 0);
        return;
    }
    ident.instrument = it->second.first;
    ident.side = it->second.second;
    auto& orders = inst_[ident.instrument].orders(ident.side);
    const auto o = std::find_if(orders.begin(), orders.end(),
                                [&](const SimOrder& x) { return x.id == req.id; });
    const Qty rem = o->remaining;
    orders.erase(o);
    live_.erase(it);
    report(ExecType::Cancelled, ident, now, RejectReason::None, rem, 0);
}

} // namespace hftbt
