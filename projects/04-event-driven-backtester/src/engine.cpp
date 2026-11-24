#include "hftbt/engine.hpp"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>

namespace hftbt {

namespace {
std::uint64_t bits(double d) noexcept { return std::bit_cast<std::uint64_t>(d); }
std::uint64_t u(std::int64_t v) noexcept { return static_cast<std::uint64_t>(v); }
} // namespace

void Engine::MdFifo::push(const Delayed& d) {
    if (tail_ - head_ == buf_.size()) {
        std::vector<Delayed> bigger(std::max<std::size_t>(1024, buf_.size() * 2));
        const std::size_t n = tail_ - head_;
        for (std::size_t i = 0; i < n; ++i) bigger[i] = buf_[(head_ + i) & mask_];
        buf_ = std::move(bigger);
        mask_ = buf_.size() - 1;
        head_ = 0;
        tail_ = n;
    }
    buf_[tail_++ & mask_] = d;
}

Engine::Engine(const FeedData& feed, const EngineConfig& cfg, Strategy& strategy)
    : feed_(feed), cfg_(cfg), strategy_(strategy),
      exchange_(feed.symbols.size(), cfg.exchange, *this), observed_(feed.symbols.size()),
      oms_(feed.symbols.size(), cfg.risk),
      portfolio_(feed.symbols.size(), cfg.fees, cfg.tick_value),
      md_path_(cfg.latency.md_delay_ns, cfg.latency.md_jitter_ns, cfg.seed ^ 0x6D64ULL),
      order_path_(cfg.latency.order_delay_ns, cfg.latency.order_jitter_ns, cfg.seed ^ 0x6F7264ULL),
      response_path_(cfg.latency.response_delay_ns, cfg.latency.response_jitter_ns,
                     cfg.seed ^ 0x72657370ULL) {
    cfg_.latency.compute_delay_ns = std::max<Timestamp>(0, cfg_.latency.compute_delay_ns);
    cfg_.sample_interval_ns = std::max<Timestamp>(1, cfg_.sample_interval_ns);
    std::erase_if(cfg_.markout_horizons_ns, [](Timestamp h) { return h <= 0; });
    summary_.markouts.resize(cfg_.markout_horizons_ns.size());
    for (std::size_t i = 0; i < cfg_.markout_horizons_ns.size(); ++i)
        summary_.markouts[i].horizon_ns = cfg_.markout_horizons_ns[i];
    if (!feed_.events.empty()) {
        now_ = feed_.events.front().ts;
        end_ts_ = feed_.events.back().ts;
    }
    if (cfg_.end_ts > 0) end_ts_ = cfg_.end_ts;
    summary_.start_ts = now_;
    if (!cfg_.event_log_path.empty()) log_ = std::fopen(cfg_.event_log_path.c_str(), "w");
}

Engine::~Engine() {
    if (log_) std::fclose(log_);
}

void Engine::push(Timestamp ts, std::uint8_t prio, Payload p) {
    heap_.push(Ev{ts, seq_++, prio, std::move(p)});
}

void Engine::run() {
    start();
    while (step()) {
    }
    finish();
}

void Engine::start() {
    if (started_) return;
    started_ = true;
    push(now_, kPrioSample, SampleEv{});
    strategy_.on_start(*this);
}

bool Engine::step() {
    if (!started_ || finished_) return false;
    enum class Src { None, Feed, Heap, Fifo } src = Src::None;
    Timestamp ts = kMaxTimestamp;
    std::uint8_t prio = 0;
    if (feed_idx_ < feed_.events.size()) {
        src = Src::Feed;
        ts = feed_.events[feed_idx_].ts;
        prio = kPrioFeed;
    }
    if (!heap_.empty()) {
        const Ev& t = heap_.top();
        if (src == Src::None || t.ts < ts || (t.ts == ts && t.prio < prio)) {
            src = Src::Heap;
            ts = t.ts;
            prio = t.prio;
        }
    }
    if (!md_fifo_.empty()) {
        const Timestamp f = md_fifo_.front().ts;
        if (src == Src::None || f < ts || (f == ts && kPrioMdObserved < prio)) {
            src = Src::Fifo;
            ts = f;
        }
    }
    if (src == Src::None || ts > end_ts_) return false;

    now_ = ts;
    ++summary_.dispatched_events;
    switch (src) {
    case Src::Feed: dispatch_feed(feed_.events[feed_idx_++]); break;
    case Src::Fifo: {
        const MarketEvent e = md_fifo_.front().ev;
        md_fifo_.pop();
        dispatch_md_observed(e);
        break;
    }
    case Src::Heap: {
        const Ev ev = heap_.top();
        heap_.pop();
        dispatch(ev);
        break;
    }
    case Src::None: break;
    }
    return true;
}

void Engine::dispatch_feed(const MarketEvent& e) {
    ++summary_.feed_events;
    mix(u(e.ts));
    mix((static_cast<std::uint64_t>(e.type) << 40) | (static_cast<std::uint64_t>(e.side) << 32) |
        e.instrument);
    mix(e.order_id);
    mix(u(e.price));
    mix(u(e.qty));
    if (log_) {
        if (e.type == MdType::Add || e.type == MdType::Level || e.type == MdType::Trade)
            std::fprintf(log_,
                         "%" PRId64 " FEED %s %c id=%" PRIu64 " side=%c px=%" PRId64 " qty=%" PRId64
                         "\n",
                         now_, feed_.symbols[e.instrument].c_str(), static_cast<char>(e.type),
                         e.order_id, e.side == Side::Buy ? 'B' : 'S', e.price, e.qty);
        else
            std::fprintf(log_, "%" PRId64 " FEED %s %c id=%" PRIu64 " qty=%" PRId64 "\n", now_,
                         feed_.symbols[e.instrument].c_str(), static_cast<char>(e.type), e.order_id,
                         e.qty);
    }
    exchange_.on_market_event(e); // truth first; may emit fills
    const Timestamp at = md_path_.deliver_at(e.ts);
    if (cfg_.md_fifo)
        md_fifo_.push(Delayed{at, e});
    else
        push(at, kPrioMdObserved, e);
}

void Engine::dispatch_md_observed(const MarketEvent& e) {
    MarketEvent ev = e;
    const ApplyResult r = observed_[ev.instrument].apply(ev);
    if (r.ok &&
        (ev.type == MdType::Cancel || ev.type == MdType::Delete || ev.type == MdType::Execute)) {
        ev.side = r.side;
        ev.price = r.price;
        ev.qty = r.qty;
    }
    mix(u(now_) ^ 0x4D44ULL);
    if (now_ < cfg_.trade_start_ts) return; // warm-up: book only
    if (log_)
        std::fprintf(log_, "%" PRId64 " MD_OBSERVED %s %c exch_ts=%" PRId64 "\n", now_,
                     feed_.symbols[ev.instrument].c_str(), static_cast<char>(ev.type), ev.ts);
    ++summary_.strategy_callbacks;
    strategy_.on_market_data(*this, ev);
}

void Engine::dispatch(const Ev& ev) {
    if (const auto* req = std::get_if<OrderRequest>(&ev.payload)) {
        mix(u(now_) ^ 0x4E4557ULL);
        mix(req->id);
        if (log_)
            std::fprintf(log_,
                         "%" PRId64 " ORDER_ARRIVE id=%" PRIu64 " %s side=%c px=%" PRId64
                         " qty=%" PRId64 "\n",
                         now_, req->id, feed_.symbols[req->instrument].c_str(),
                         req->side == Side::Buy ? 'B' : 'S', req->price, req->qty);
        exchange_.on_new_order(*req, now_);
    } else if (const auto* cxl = std::get_if<CancelRequest>(&ev.payload)) {
        mix(u(now_) ^ 0x43584CULL);
        mix(cxl->id);
        if (log_) std::fprintf(log_, "%" PRId64 " CANCEL_ARRIVE id=%" PRIu64 "\n", now_, cxl->id);
        exchange_.on_cancel(*cxl, now_);
    } else if (const auto* md = std::get_if<MarketEvent>(&ev.payload)) {
        dispatch_md_observed(*md);
    } else if (const auto* rep = std::get_if<ExecReport>(&ev.payload)) {
        mix(u(now_) ^ 0x525350ULL);
        mix(rep->order_id);
        if (log_)
            std::fprintf(log_,
                         "%" PRId64 " REPORT_OBSERVED id=%" PRIu64 " %s exch_ts=%" PRId64 "\n",
                         now_, rep->order_id, to_string(rep->type), rep->exch_ts);
        oms_.on_report(*rep);
        ++summary_.strategy_callbacks;
        strategy_.on_order_update(*this, *rep);
    } else if (const auto* tm = std::get_if<TimerEv>(&ev.payload)) {
        mix(u(now_) ^ 0x544DULL);
        mix(tm->id);
        if (log_) std::fprintf(log_, "%" PRId64 " TIMER id=%" PRIu64 "\n", now_, tm->id);
        ++summary_.strategy_callbacks;
        strategy_.on_timer(*this, tm->id);
    } else if (const auto* mo = std::get_if<MarkoutEv>(&ev.payload)) {
        // Adverse selection: where is the (true) mid a horizon after the fill?
        if (const auto mid = exchange_.book(mo->instrument).mid()) {
            const double m = sign(mo->side) * (*mid - static_cast<double>(mo->price));
            MarkoutStats& st = summary_.markouts[mo->horizon];
            (mo->maker ? st.maker_sum : st.taker_sum) += m * static_cast<double>(mo->qty);
            (mo->maker ? st.maker_qty : st.taker_qty) += mo->qty;
            ++markouts_resolved_;
        }
    } else {
        sample();
        if (now_ < end_ts_)
            push(std::min(end_ts_, now_ + cfg_.sample_interval_ns), kPrioSample, SampleEv{});
    }
}

void Engine::on_exec_report(const ExecReport& r) {
    mix(u(r.exch_ts));
    mix(r.order_id);
    mix((static_cast<std::uint64_t>(r.type) << 8) | static_cast<std::uint64_t>(r.maker));
    mix(u(r.price));
    mix(u(r.qty));
    mix(u(r.leaves));
    if (log_)
        std::fprintf(log_,
                     "%" PRId64 " EXCH_%s id=%" PRIu64 " px=%" PRId64 " qty=%" PRId64
                     " leaves=%" PRId64 "%s\n",
                     now_, to_string(r.type), r.order_id, r.price, r.qty, r.leaves,
                     r.type == ExecType::Fill ? (r.maker ? " maker" : " taker") : "");
    switch (r.type) {
    case ExecType::Fill: {
        const double fee = portfolio_.on_fill(r);
        ++summary_.fills;
        ++(r.maker ? summary_.maker_fills : summary_.taker_fills);
        summary_.qty_filled += r.qty;
        if (r.leaves == 0) ++summary_.orders_fully_filled;
        if (cfg_.record_fills)
            fills_.push_back(
                {r.exch_ts, r.order_id, r.instrument, r.side, r.maker, r.price, r.qty, fee});
        for (std::uint32_t h = 0; h < cfg_.markout_horizons_ns.size(); ++h) {
            ++markouts_scheduled_;
            push(now_ + cfg_.markout_horizons_ns[h], kPrioMarkout,
                 MarkoutEv{r.instrument, r.side, r.maker, r.price, r.qty, h});
        }
        break;
    }
    case ExecType::Rejected: ++summary_.orders_exchange_rejected; break;
    case ExecType::CancelRejected: ++summary_.cancel_rejects; break;
    case ExecType::Ack:
    case ExecType::Cancelled: break;
    }
    // The strategy learns about it only after the response latency.
    push(response_path_.deliver_at(now_), kPrioResponse, r);
}

SendResult Engine::submit(OrderRequest req) {
    req.id = next_order_id_++;
    const RejectReason why = oms_.try_register(req);
    if (why != RejectReason::None) {
        ++summary_.orders_risk_rejected;
        return {0, why};
    }
    ++summary_.orders_sent;
    summary_.qty_submitted += req.qty;
    push(order_path_.deliver_at(now_ + cfg_.latency.compute_delay_ns), kPrioOrderEntry, req);
    return {req.id, RejectReason::None};
}

SendResult Engine::send_limit(InstrumentId i, Side side, Price price, Qty qty, Tif tif,
                              bool post_only) {
    OrderRequest req;
    req.instrument = i;
    req.side = side;
    req.type = OrdType::Limit;
    req.tif = tif;
    req.post_only = post_only;
    req.price = price;
    req.qty = qty;
    if (price <= 0) {
        ++summary_.orders_risk_rejected;
        return {0, RejectReason::InvalidOrder};
    }
    return submit(req);
}

SendResult Engine::send_market(InstrumentId i, Side side, Qty qty) {
    OrderRequest req;
    req.instrument = i;
    req.side = side;
    req.type = OrdType::Market;
    req.tif = Tif::IOC;
    req.qty = qty;
    return submit(req);
}

bool Engine::cancel(OrderId id) {
    const OmsOrder* o = oms_.find(id);
    if (!o) return false;
    const InstrumentId instr = o->instrument;
    if (!oms_.try_cancel(id)) return false;
    ++summary_.cancels_sent;
    push(order_path_.deliver_at(now_ + cfg_.latency.compute_delay_ns), kPrioOrderEntry,
         CancelRequest{id, instr});
    return true;
}

std::uint64_t Engine::set_timer(Timestamp at) {
    const std::uint64_t id = next_timer_id_++;
    push(std::max(at, now_), kPrioTimer, TimerEv{id});
    return id;
}

void Engine::sample() {
    Qty net = 0;
    for (InstrumentId i = 0; i < portfolio_.size(); ++i) {
        if (const auto mid = exchange_.book(i).mid()) portfolio_.set_mark(i, *mid);
        net += portfolio_.account(i).position;
    }
    const double eq = portfolio_.equity();
    const double gross = portfolio_.gross_exposure();
    if (!equity_.empty()) // left Riemann sum: exposure held since the previous sample
        exposure_integral_ +=
            equity_.back().gross_exposure * static_cast<double>(now_ - equity_.back().ts);
    peak_equity_ = equity_.empty() ? eq : std::max(peak_equity_, eq);
    summary_.max_drawdown = std::max(summary_.max_drawdown, peak_equity_ - eq);
    summary_.max_gross_exposure = std::max(summary_.max_gross_exposure, gross);
    equity_.push_back({now_, eq, gross, net});
}

void Engine::finish() {
    if (!started_ || finished_) return;
    finished_ = true;
    if (equity_.empty() || equity_.back().ts != now_) sample();
    strategy_.on_stop(*this);

    Summary& s = summary_;
    s.end_ts = now_;
    s.data_anomalies = exchange_.anomalies();
    s.fill_ratio = s.qty_submitted > 0
                       ? static_cast<double>(s.qty_filled) / static_cast<double>(s.qty_submitted)
                       : 0.0;
    s.pnl_net = portfolio_.equity();
    s.fees = portfolio_.total_fees();
    s.pnl_gross = s.pnl_net + s.fees;
    s.turnover = portfolio_.total_notional();
    s.open_orders_at_end = exchange_.open_orders();
    s.markouts_unresolved = markouts_scheduled_ - markouts_resolved_;
    const Timestamp span = equity_.back().ts - equity_.front().ts;
    s.avg_gross_exposure =
        span > 0 ? exposure_integral_ / static_cast<double>(span) : equity_.back().gross_exposure;
    for (InstrumentId i = 0; i < portfolio_.size(); ++i) {
        const InstrumentAccount& a = portfolio_.account(i);
        s.final_positions.push_back(a.position);
        s.realized += a.realized;
        s.unrealized += portfolio_.unrealized(i);
        mix(u(a.position));
        mix(bits(a.cash));
    }
    mix(bits(s.pnl_net));
    s.event_hash = hash_;
    if (log_) std::fflush(log_);
}

} // namespace hftbt
