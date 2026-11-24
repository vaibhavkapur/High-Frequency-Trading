#include "hftbt/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace hftbt {

Portfolio::Portfolio(std::size_t n_instruments, const FeeConfig& fees, double tick_value)
    : acct_(n_instruments), fees_(fees), tick_value_(tick_value) {}

double Portfolio::on_fill(const ExecReport& f) {
    InstrumentAccount& a = acct_[f.instrument];
    const double px = static_cast<double>(f.price);
    const double notional = px * static_cast<double>(f.qty) * tick_value_;
    const double fee = notional * (f.maker ? fees_.maker_bps : fees_.taker_bps) * 1e-4;
    const Qty signed_qty = sign(f.side) * f.qty;

    a.cash -= static_cast<double>(signed_qty) * px * tick_value_ + fee;
    a.fees += fee;
    a.notional += notional;
    a.volume += f.qty;
    ++(f.maker ? a.maker_fills : a.taker_fills);

    // Average-cost accounting for the realized / unrealized split.
    if (a.position == 0 || (a.position > 0) == (signed_qty > 0)) {
        const double total = static_cast<double>(std::abs(a.position) + f.qty);
        a.avg_cost = (a.avg_cost * static_cast<double>(std::abs(a.position)) +
                      px * static_cast<double>(f.qty)) /
                     total;
        a.position += signed_qty;
    } else {
        const Qty closing = std::min<Qty>(std::abs(a.position), f.qty);
        const int dir = a.position > 0 ? 1 : -1;
        a.realized += dir * (px - a.avg_cost) * static_cast<double>(closing) * tick_value_;
        a.position += signed_qty;
        if (a.position == 0)
            a.avg_cost = 0.0;
        else if ((a.position > 0 ? 1 : -1) != dir)
            a.avg_cost = px; // flipped through zero: remainder opens at fill price
    }
    if (!a.has_mark) set_mark(f.instrument, px);
    return fee;
}

double Portfolio::unrealized(InstrumentId i) const noexcept {
    const InstrumentAccount& a = acct_[i];
    return static_cast<double>(a.position) * (a.mark - a.avg_cost) * tick_value_;
}

double Portfolio::equity() const noexcept {
    double e = 0.0;
    for (const InstrumentAccount& a : acct_)
        e += a.cash + static_cast<double>(a.position) * a.mark * tick_value_;
    return e;
}

double Portfolio::pnl_decomposed() const noexcept {
    double e = 0.0;
    for (std::size_t i = 0; i < acct_.size(); ++i)
        e += acct_[i].realized + unrealized(static_cast<InstrumentId>(i)) - acct_[i].fees;
    return e;
}

double Portfolio::gross_exposure() const noexcept {
    double e = 0.0;
    for (const InstrumentAccount& a : acct_)
        e += std::abs(static_cast<double>(a.position) * a.mark * tick_value_);
    return e;
}

double Portfolio::total_fees() const noexcept {
    double e = 0.0;
    for (const InstrumentAccount& a : acct_) e += a.fees;
    return e;
}

double Portfolio::total_notional() const noexcept {
    double e = 0.0;
    for (const InstrumentAccount& a : acct_) e += a.notional;
    return e;
}

Qty Portfolio::total_volume() const noexcept {
    Qty v = 0;
    for (const InstrumentAccount& a : acct_) v += a.volume;
    return v;
}

} // namespace hftbt
