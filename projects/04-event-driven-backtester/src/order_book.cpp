#include "hftbt/order_book.hpp"

#include <algorithm>

namespace hftbt {

Level* BookSide::find(Price p) noexcept {
    // Scan from the touch outward: activity concentrates near the best price.
    for (std::size_t i = lv_.size(); i-- > 0;) {
        if (lv_[i].price == p) return &lv_[i];
        if (better(side_, p, lv_[i].price)) return nullptr;
    }
    return nullptr;
}

Level& BookSide::get_or_insert(Price p) {
    std::size_t i = lv_.size();
    while (i > 0) {
        const Price q = lv_[i - 1].price;
        if (q == p) return lv_[i - 1];
        if (better(side_, p, q)) break; // lv_[i-1] is worse than p: insert after it
        --i;
    }
    return *lv_.insert(lv_.begin() + static_cast<std::ptrdiff_t>(i), Level{p, 0, 0, 0});
}

void BookSide::erase(Price p) noexcept {
    if (Level* l = find(p)) lv_.erase(lv_.begin() + (l - lv_.data()));
}

// `consumed` tracks displayed lots our own taker orders already took. We
// model those as the front of the queue: a historical execution removes them
// (they are gone for everyone), anything else merely caps them at what is
// still displayed. New liquidity never resets it.
void OrderBook::remove_qty(Side s, Price p, Qty q, bool executed, ApplyResult& r) {
    BookSide& bs = side_mut(s);
    Level* l = bs.find(p);
    if (!l) return;
    r.before = l->qty;
    l->qty = std::max<Qty>(0, l->qty - q);
    if (executed) l->consumed = std::max<Qty>(0, l->consumed - q);
    l->consumed = std::min(l->consumed, l->qty);
    r.after = l->qty;
    if (l->qty == 0) bs.erase(p);
}

ApplyResult OrderBook::apply(const MarketEvent& e) {
    ApplyResult r;
    switch (e.type) {
    case MdType::Add: {
        if (e.qty <= 0 || e.price <= 0) break;
        const auto [it, inserted] =
            orders_.try_emplace(e.order_id, L3Order{e.price, e.qty, seq_, e.side});
        if (!inserted) break;
        Level& l = side_mut(e.side).get_or_insert(e.price);
        r.ok = true;
        r.side = e.side;
        r.price = e.price;
        r.qty = e.qty;
        r.before = l.qty;
        l.qty += e.qty;
        r.after = l.qty;
        r.seq = seq_++;
        return r;
    }
    case MdType::Cancel:
    case MdType::Delete:
    case MdType::Execute: {
        if (e.type != MdType::Delete && e.qty <= 0) break;
        const auto it = orders_.find(e.order_id);
        if (it == orders_.end()) break;
        L3Order& o = it->second;
        const Qty q = e.type == MdType::Delete ? o.qty : std::min(e.qty, o.qty);
        r.ok = true;
        r.side = o.side;
        r.price = o.price;
        r.qty = q;
        r.seq = o.seq;
        remove_qty(o.side, o.price, q, e.type == MdType::Execute, r);
        o.qty -= q;
        if (o.qty == 0) orders_.erase(it);
        return r;
    }
    case MdType::Level: {
        if (e.qty < 0 || e.price <= 0) break;
        BookSide& bs = side_mut(e.side);
        Level* l = bs.find(e.price);
        r.ok = true;
        r.side = e.side;
        r.price = e.price;
        r.before = l ? l->qty : 0;
        r.pending_trade = l ? l->pending_trade : 0;
        r.after = e.qty;
        r.qty = r.after > r.before ? r.after - r.before : r.before - r.after;
        if (e.qty == 0) {
            if (l) bs.erase(e.price);
        } else {
            Level& ll = l ? *l : bs.get_or_insert(e.price);
            if (r.before > r.after) // the traded part of the decrease came off the front
                ll.consumed -= std::min({ll.consumed, r.before - r.after, r.pending_trade});
            ll.consumed = std::min(ll.consumed, e.qty);
            ll.qty = e.qty;
            ll.pending_trade = 0;
        }
        return r;
    }
    case MdType::Trade: {
        if (e.qty <= 0 || e.price <= 0) break;
        r.ok = true;
        r.side = opposite(e.side); // resting side that was hit
        r.price = e.price;
        r.qty = e.qty;
        if (Level* l = side_mut(r.side).find(e.price)) {
            l->pending_trade += e.qty;
            r.before = r.after = l->qty;
        }
        return r;
    }
    }
    ++anomalies_;
    return r;
}

} // namespace hftbt
