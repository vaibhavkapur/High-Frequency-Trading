#include "hftbt/strategies.hpp"

#include <algorithm>
#include <cmath>

namespace hftbt {

namespace {
bool is_terminal(const ExecReport& r) noexcept {
    return r.type == ExecType::Cancelled || r.type == ExecType::Rejected ||
           (r.type == ExecType::Fill && r.leaves == 0);
}
} // namespace

// ---------------------------------------------------------------- MarketMaker

void MarketMaker::on_start(Context& ctx) { st_.assign(ctx.num_instruments(), State{}); }

void MarketMaker::on_market_data(Context& ctx, const MarketEvent& ev) {
    requote(ctx, ev.instrument);
}

void MarketMaker::on_order_update(Context& ctx, const ExecReport& r) {
    if (r.instrument >= st_.size()) return;
    if (is_terminal(r))
        for (Quote& q : st_[r.instrument].q)
            if (q.id == r.order_id) {
                q.id = 0;
                q.cancelling = false;
            }
    requote(ctx, r.instrument);
}

void MarketMaker::requote(Context& ctx, InstrumentId i) {
    const OrderBook& book = ctx.book(i);
    const Level* bb = book.best_bid();
    const Level* ba = book.best_ask();
    if (!bb || !ba || bb->price >= ba->price) { // no usable market: pull quotes
        manage(ctx, i, Side::Buy, false, 0, 0);
        manage(ctx, i, Side::Sell, false, 0, 0);
        return;
    }
    const Qty pos = ctx.position(i);
    const double mid = 0.5 * static_cast<double>(bb->price + ba->price);
    const double reservation = mid - p_.skew_ticks_per_lot * static_cast<double>(pos);
    const double hs = static_cast<double>(p_.half_spread_ticks);
    // Never quote through the opposite touch: we intend to be passive.
    const Price bid =
        std::min<Price>(static_cast<Price>(std::floor(reservation - hs)), ba->price - 1);
    const Price ask =
        std::max<Price>(static_cast<Price>(std::ceil(reservation + hs)), bb->price + 1);
    const Qty bid_qty = std::min(p_.order_qty, p_.max_position - pos);
    const Qty ask_qty = std::min(p_.order_qty, p_.max_position + pos);
    manage(ctx, i, Side::Buy, bid_qty > 0 && bid > 0, bid, bid_qty);
    manage(ctx, i, Side::Sell, ask_qty > 0, ask, ask_qty);
}

void MarketMaker::manage(Context& ctx, InstrumentId i, Side side, bool want, Price px, Qty qty) {
    Quote& q = st_[i].q[static_cast<int>(side)];
    const Timestamp now = ctx.now();
    const bool throttled = now - q.last_action < p_.min_requote_interval_ns;
    if (q.id != 0) {
        const bool stale = !want || std::abs(q.px - px) >= p_.requote_threshold_ticks;
        if (stale && !q.cancelling && !throttled && ctx.cancel(q.id)) {
            q.cancelling = true;
            q.last_action = now;
        }
        return; // replacement goes out once the cancel (or a fill) is observed
    }
    if (!want || throttled) return;
    if (const SendResult r = ctx.send_limit(i, side, px, qty, Tif::GTC, p_.post_only)) {
        q.id = r.id;
        q.px = px;
        q.cancelling = false;
        q.last_action = now;
    }
}

// ------------------------------------------------------------- ImbalanceTaker

void ImbalanceTaker::on_start(Context& ctx) { st_.assign(ctx.num_instruments(), State{}); }

void ImbalanceTaker::on_market_data(Context& ctx, const MarketEvent& ev) {
    const InstrumentId i = ev.instrument;
    State& s = st_[i];
    const Timestamp now = ctx.now();
    if (s.pending != 0 || now - s.last_send < p_.cooldown_ns) return;
    const OrderBook& book = ctx.book(i);
    const Level* bb = book.best_bid();
    const Level* ba = book.best_ask();
    if (!bb || !ba || bb->price >= ba->price) return;
    const Qty pos = ctx.position(i);

    SendResult r;
    if (p_.max_hold_ns > 0 && pos != 0 && now - s.entry_ts >= p_.max_hold_ns) {
        r = pos > 0 ? ctx.send_limit(i, Side::Sell, bb->price, pos, Tif::IOC)
                    : ctx.send_limit(i, Side::Buy, ba->price, -pos, Tif::IOC);
    } else {
        const double imb =
            static_cast<double>(bb->qty - ba->qty) / static_cast<double>(bb->qty + ba->qty);
        if (imb > p_.threshold && pos < p_.max_position)
            r = ctx.send_limit(i, Side::Buy, ba->price,
                               std::min(p_.order_qty, p_.max_position - pos), Tif::IOC);
        else if (imb < -p_.threshold && pos > -p_.max_position)
            r = ctx.send_limit(i, Side::Sell, bb->price,
                               std::min(p_.order_qty, p_.max_position + pos), Tif::IOC);
    }
    if (r) {
        s.pending = r.id;
        s.last_send = now;
    }
}

void ImbalanceTaker::on_order_update(Context& ctx, const ExecReport& r) {
    if (r.instrument >= st_.size()) return;
    State& s = st_[r.instrument];
    if (r.type == ExecType::Fill) {
        // Restart the holding clock whenever a fill adds to the position.
        const Qty pos = ctx.position(r.instrument);
        if (pos != 0 && (pos > 0) == (r.side == Side::Buy)) s.entry_ts = ctx.now();
    }
    if (r.order_id == s.pending && is_terminal(r)) s.pending = 0;
}

// --------------------------------------------------------------------- Ladder

void Ladder::on_start(Context& ctx) { placed_.assign(ctx.num_instruments(), false); }

void Ladder::on_market_data(Context& ctx, const MarketEvent& ev) {
    const InstrumentId i = ev.instrument;
    if (placed_[i]) return;
    const OrderBook& book = ctx.book(i);
    const Level* bb = book.best_bid();
    const Level* ba = book.best_ask();
    if (!bb || !ba) return;
    placed_[i] = true;
    for (std::uint32_t k = 0; k < p_.levels; ++k) {
        ctx.send_limit(i, Side::Buy, bb->price - p_.offset_ticks - k, p_.order_qty);
        ctx.send_limit(i, Side::Sell, ba->price + p_.offset_ticks + k, p_.order_qty);
    }
}

} // namespace hftbt
