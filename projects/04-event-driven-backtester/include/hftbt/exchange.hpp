// Simulated exchange: replays historical events into the truth book and
// decides when simulated orders fill. Our orders never alter the historical
// book (no market impact) -- see docs/assumptions.md.
#pragma once

#include <unordered_map>
#include <vector>

#include "hftbt/order.hpp"
#include "hftbt/order_book.hpp"

namespace hftbt {

// How L2 cancellations (level decreases not explained by trades) move a
// simulated order's queue position. L3 feeds do not need this: the position
// of every cancelled order is known.
enum class QueueModel : std::uint8_t {
    Conservative, // cancels come from behind us whenever possible
    Proportional, // cancels are spread uniformly over the queue
    Optimistic,   // cancels come from ahead of us first
};

struct ExchangeConfig {
    QueueModel queue_model = QueueModel::Conservative;
    // Fill a resting simulated order when historical liquidity is posted at a
    // price that crosses it (the poster would have traded with us).
    bool fill_on_crossing_quote = true;
    // Extra adverse ticks applied to every taker fill price.
    Price taker_slippage_ticks = 0;
};

class ExecListener {
public:
    virtual ~ExecListener() = default;
    virtual void on_exec_report(const ExecReport& r) = 0;
};

struct SimOrder {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    Qty remaining = 0;
    Qty ahead = 0;              // historical qty that must trade before us
    std::uint64_t join_seq = 0; // L3 orders with seq < join_seq are ahead
};

class Exchange {
public:
    Exchange(std::size_t n_instruments, const ExchangeConfig& cfg, ExecListener& listener);

    // Historical event at exchange time e.ts.
    void on_market_event(const MarketEvent& e);
    // Order-entry messages, at the time they ARRIVE at the exchange.
    void on_new_order(const OrderRequest& req, Timestamp now);
    void on_cancel(const CancelRequest& req, Timestamp now);

    const OrderBook& book(InstrumentId i) const { return inst_[i].book; }
    std::size_t num_instruments() const noexcept { return inst_.size(); }
    std::size_t open_orders() const noexcept { return live_.size(); }
    const SimOrder* find(OrderId id) const;
    std::uint64_t anomalies() const noexcept;

private:
    struct Instrument {
        OrderBook book;
        // Each sorted most-aggressive price first, then by arrival.
        std::vector<SimOrder> bids;
        std::vector<SimOrder> asks;
        std::vector<SimOrder>& orders(Side s) { return s == Side::Buy ? bids : asks; }
    };
    enum class Passive { L3Execute, L2Trade, CrossingQuote };

    void match_passive(Instrument& st, InstrumentId instr, Side our_side, Price px, Qty qty,
                       std::uint64_t seq, Passive kind, Timestamp now);
    void reduce_ahead_l3(Instrument& st, Side s, Price px, Qty removed, std::uint64_t seq);
    void reduce_ahead_l2(Instrument& st, Side s, Price px, Qty base, Qty cancelled, Qty after);
    void report(ExecType type, const OrderRequest& req, Timestamp now, RejectReason why, Qty qty,
                Qty leaves);

    ExchangeConfig cfg_;
    ExecListener& listener_;
    std::vector<Instrument> inst_;
    std::unordered_map<OrderId, std::pair<InstrumentId, Side>> live_;
};

} // namespace hftbt
