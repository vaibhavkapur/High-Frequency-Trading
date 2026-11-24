// Aggregated price-level book with optional order-level (L3) tracking.
// Used twice per instrument: once as exchange truth, once as the delayed
// view the strategy is allowed to see.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "hftbt/market_event.hpp"

namespace hftbt {

struct Level {
    Price price = 0;
    Qty qty = 0;
    Qty pending_trade = 0; // L2: traded qty not yet reflected by a Level update
    Qty consumed = 0;      // exchange-truth only: qty our own taker orders removed
};

class BookSide {
public:
    explicit BookSide(Side side) : side_(side) {}

    bool empty() const noexcept { return lv_.empty(); }
    std::size_t size() const noexcept { return lv_.size(); }
    const Level* best() const noexcept { return lv_.empty() ? nullptr : &lv_.back(); }
    // i == 0 is the best level.
    const Level& from_best(std::size_t i) const noexcept { return lv_[lv_.size() - 1 - i]; }
    Level& from_best(std::size_t i) noexcept { return lv_[lv_.size() - 1 - i]; }

    Level* find(Price p) noexcept;
    const Level* find(Price p) const noexcept { return const_cast<BookSide*>(this)->find(p); }
    Level& get_or_insert(Price p);
    void erase(Price p) noexcept;
    Qty qty_at(Price p) const noexcept {
        const Level* l = find(p);
        return l ? l->qty : 0;
    }

private:
    Side side_;
    std::vector<Level> lv_; // sorted worst -> best, so the touch is at the back
};

// What a market event did to the book; the exchange simulator derives queue
// updates and fills for simulated orders from this.
struct ApplyResult {
    bool ok = false;       // false: event was inconsistent and ignored
    Side side = Side::Buy; // RESTING side affected
    Price price = 0;
    Qty qty = 0;           // qty added / removed / traded (clamped to what existed)
    Qty before = 0;        // level qty before
    Qty after = 0;         // level qty after
    Qty pending_trade = 0; // Level events: traded qty awaiting this update
    std::uint64_t seq = 0; // arrival sequence of the L3 order involved
};

class OrderBook {
public:
    OrderBook() : bids_(Side::Buy), asks_(Side::Sell) {}

    ApplyResult apply(const MarketEvent& e);

    const BookSide& side(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }
    BookSide& side_mut(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    const Level* best_bid() const noexcept { return bids_.best(); }
    const Level* best_ask() const noexcept { return asks_.best(); }
    // Mid in ticks; empty unless both sides are present.
    std::optional<double> mid() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return 0.5 * static_cast<double>(bids_.best()->price + asks_.best()->price);
    }

    // Sequence the next L3 Add will receive. An order with seq < this value
    // was in the book before "now".
    std::uint64_t next_seq() const noexcept { return seq_; }
    std::size_t l3_orders() const noexcept { return orders_.size(); }
    std::uint64_t anomalies() const noexcept { return anomalies_; }

private:
    struct L3Order {
        Price price;
        Qty qty;
        std::uint64_t seq;
        Side side;
    };
    void remove_qty(Side s, Price p, Qty q, bool executed, ApplyResult& r);

    BookSide bids_;
    BookSide asks_;
    std::unordered_map<OrderId, L3Order> orders_;
    std::uint64_t seq_ = 1;
    std::uint64_t anomalies_ = 0;
};

} // namespace hftbt
