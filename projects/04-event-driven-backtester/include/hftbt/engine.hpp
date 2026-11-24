// The event loop. Owns exchange truth, the strategy's delayed view, and the
// scheduler that keeps the two causally separated.
#pragma once

#include <cstdio>
#include <queue>
#include <string>
#include <variant>
#include <vector>

#include "hftbt/exchange.hpp"
#include "hftbt/feed.hpp"
#include "hftbt/latency.hpp"
#include "hftbt/portfolio.hpp"
#include "hftbt/strategy.hpp"

namespace hftbt {

struct EngineConfig {
    LatencyConfig latency;
    ExchangeConfig exchange;
    FeeConfig fees;
    RiskConfig risk;
    double tick_value = 1.0;                      // currency per tick per lot
    std::uint64_t seed = 1;                       // drives latency jitter
    Timestamp sample_interval_ns = 1'000'000'000; // equity curve resolution
    std::vector<Timestamp> markout_horizons_ns;   // adverse-selection horizons
    Timestamp end_ts = 0;                         // 0: stop at the last feed event
    // Market data before this time still builds both books but is not
    // delivered to the strategy. Lets an out-of-sample run start mid-file
    // with a correct book and a flat position.
    Timestamp trade_start_ts = 0;
    // true: delayed market data waits in a FIFO merged with the event heap.
    // false: it goes through the heap like everything else. Identical
    // results (tested); different speed (benchmarked).
    bool md_fifo = true;
    bool record_fills = true;
    std::string event_log_path; // optional text trace of every dispatched event
};

struct FillRecord {
    Timestamp exch_ts;
    OrderId order_id;
    InstrumentId instrument;
    Side side;
    bool maker;
    Price price;
    Qty qty;
    double fee;
};

struct EquitySample {
    Timestamp ts;
    double equity;
    double gross_exposure;
    Qty net_position;
};

struct MarkoutStats {
    Timestamp horizon_ns = 0;
    double maker_sum = 0.0; // sum(qty * signed markout in ticks)
    Qty maker_qty = 0;
    double taker_sum = 0.0;
    Qty taker_qty = 0;
};

struct Summary {
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;
    std::uint64_t feed_events = 0;
    std::uint64_t dispatched_events = 0;
    std::uint64_t strategy_callbacks = 0;
    std::uint64_t data_anomalies = 0;

    std::uint64_t orders_sent = 0;
    std::uint64_t orders_risk_rejected = 0;
    std::uint64_t orders_exchange_rejected = 0;
    std::uint64_t orders_fully_filled = 0;
    std::uint64_t cancels_sent = 0;
    std::uint64_t cancel_rejects = 0;
    std::uint64_t fills = 0;
    std::uint64_t maker_fills = 0;
    std::uint64_t taker_fills = 0;
    Qty qty_submitted = 0;
    Qty qty_filled = 0;
    double fill_ratio = 0.0; // qty_filled / qty_submitted

    double pnl_net = 0.0;   // final equity (starting equity is 0)
    double pnl_gross = 0.0; // net + fees
    double fees = 0.0;
    double realized = 0.0;
    double unrealized = 0.0;
    double turnover = 0.0; // traded notional
    double max_drawdown = 0.0;
    double avg_gross_exposure = 0.0; // time-weighted over equity samples
    double max_gross_exposure = 0.0;
    std::vector<Qty> final_positions;
    std::vector<MarkoutStats> markouts;
    std::uint64_t markouts_unresolved = 0;
    std::size_t open_orders_at_end = 0;

    std::uint64_t event_hash = 0; // digest of every dispatched event + final state
};

class Engine final : public Context, private ExecListener {
public:
    Engine(const FeedData& feed, const EngineConfig& cfg, Strategy& strategy);
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void run(); // start(); while (step()); finish();
    void start();
    bool step(); // dispatch exactly one event; false when the run is over
    void finish();

    const Summary& summary() const noexcept { return summary_; }
    const std::vector<FillRecord>& fills() const noexcept { return fills_; }
    const std::vector<EquitySample>& equity_curve() const noexcept { return equity_; }
    const Portfolio& portfolio() const noexcept { return portfolio_; }
    const Exchange& exchange() const noexcept { return exchange_; }
    const FeedData& feed() const noexcept { return feed_; }

    // Context
    Timestamp now() const override { return now_; }
    std::size_t num_instruments() const override { return feed_.symbols.size(); }
    const std::string& symbol(InstrumentId i) const override { return feed_.symbols[i]; }
    const OrderBook& book(InstrumentId i) const override { return observed_[i]; }
    const Oms& oms() const override { return oms_; }
    SendResult send_limit(InstrumentId i, Side side, Price price, Qty qty, Tif tif = Tif::GTC,
                          bool post_only = false) override;
    SendResult send_market(InstrumentId i, Side side, Qty qty) override;
    bool cancel(OrderId id) override;
    std::uint64_t set_timer(Timestamp at) override;

private:
    struct TimerEv {
        std::uint64_t id;
    };
    struct MarkoutEv {
        InstrumentId instrument;
        Side side;
        bool maker;
        Price price;
        Qty qty;
        std::uint32_t horizon;
    };
    struct SampleEv {};
    using Payload = std::variant<OrderRequest, CancelRequest, MarketEvent, ExecReport, TimerEv,
                                 MarkoutEv, SampleEv>;
    // Tie-break at equal timestamps. Feed events (priority 0) never enter the heap.
    enum Prio : std::uint8_t {
        kPrioFeed = 0,
        kPrioOrderEntry = 1,
        kPrioMdObserved = 2,
        kPrioResponse = 3,
        kPrioTimer = 4,
        kPrioMarkout = 5,
        kPrioSample = 6,
    };
    struct Ev {
        Timestamp ts;
        std::uint64_t seq;
        std::uint8_t prio;
        Payload payload;
    };
    struct EvLater {
        bool operator()(const Ev& a, const Ev& b) const noexcept {
            if (a.ts != b.ts) return a.ts > b.ts;
            if (a.prio != b.prio) return a.prio > b.prio;
            return a.seq > b.seq;
        }
    };
    struct Delayed {
        Timestamp ts;
        MarketEvent ev;
    };
    // Growable power-of-two ring buffer.
    class MdFifo {
    public:
        bool empty() const noexcept { return head_ == tail_; }
        const Delayed& front() const noexcept { return buf_[head_ & mask_]; }
        void pop() noexcept { ++head_; }
        void push(const Delayed& d);

    private:
        std::vector<Delayed> buf_;
        std::size_t mask_ = 0;
        std::size_t head_ = 0;
        std::size_t tail_ = 0;
    };

    void on_exec_report(const ExecReport& r) override;
    void push(Timestamp ts, std::uint8_t prio, Payload p);
    void dispatch_feed(const MarketEvent& e);
    void dispatch_md_observed(const MarketEvent& e);
    void dispatch(const Ev& ev);
    SendResult submit(OrderRequest req);
    void sample();
    void mix(std::uint64_t v) noexcept {
        hash_ = (hash_ ^ v) * 0x9E3779B97F4A7C15ULL;
        hash_ ^= hash_ >> 29;
    }

    const FeedData& feed_;
    EngineConfig cfg_;
    Strategy& strategy_;

    Exchange exchange_;               // truth
    std::vector<OrderBook> observed_; // what the strategy may see
    Oms oms_;
    Portfolio portfolio_;
    LatencyPath md_path_;
    LatencyPath order_path_;
    LatencyPath response_path_;

    std::priority_queue<Ev, std::vector<Ev>, EvLater> heap_;
    MdFifo md_fifo_;
    std::size_t feed_idx_ = 0;
    std::uint64_t seq_ = 0;
    Timestamp now_ = 0;
    Timestamp end_ts_ = 0;
    OrderId next_order_id_ = 1;
    std::uint64_t next_timer_id_ = 1;
    bool started_ = false;
    bool finished_ = false;

    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
    std::uint64_t markouts_scheduled_ = 0;
    std::uint64_t markouts_resolved_ = 0;
    double peak_equity_ = 0.0;
    double exposure_integral_ = 0.0;
    Summary summary_;
    std::vector<FillRecord> fills_;
    std::vector<EquitySample> equity_;
    std::FILE* log_ = nullptr;
};

// Writes summary.json, fills.csv and equity.csv into `dir` (created if needed).
bool write_reports(const Engine& engine, const std::string& dir, std::string& err);
std::string summary_json(const Summary& s, const std::vector<std::string>& symbols);

} // namespace hftbt
