#include "numa_engine/matching_engine.hpp"

#include <algorithm>
#include <sstream>

namespace numa_engine {

MatchingEngine::MatchingEngine() noexcept {
    reset();
}

void MatchingEngine::reset() noexcept {
    books_.fill(SymbolBookState{});
    total_events_ = 0;
    applied_events_ = 0;
    noop_events_ = 0;
    add_events_ = 0;
    modify_events_ = 0;
    cancel_events_ = 0;
    trade_events_ = 0;
    last_seq_id_ = 0;
    total_volume_ = 0;
    total_notional_ticks_ = 0;
    state_digest_ = 14695981039346656037ULL;
    sequence_strictly_monotonic_ = true;
}

void MatchingEngine::fold_digest(std::uint64_t value) noexcept {
    // 64-bit FNV-1a mix of 8 bytes
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU);
        state_digest_ ^= static_cast<std::uint64_t>(byte);
        state_digest_ *= kPrime;
    }
}

ProcessedEvent MatchingEngine::process(
    const MarketEvent& ev,
    std::uint64_t recv_ts_ns,
    std::uint64_t done_ts_ns) noexcept {
    ++total_events_;
    if (ev.seq_id <= last_seq_id_) {
        sequence_strictly_monotonic_ = false;
    }
    last_seq_id_ = ev.seq_id;

    const std::uint16_t sym = static_cast<std::uint16_t>(ev.symbol_id % kMaxSymbols);
    auto& book = books_[sym];
    ++book.event_count;

    std::uint8_t status = 0;  // 0 = applied, 1 = noop
    std::uint32_t accepted_qty = ev.quantity;

    const auto type = static_cast<EventType>(ev.event_type);
    const auto side = static_cast<Side>(ev.side);

    switch (type) {
        case EventType::kAdd:
            ++add_events_;
            if (side == Side::kBid) {
                if (ev.price_ticks > book.best_bid_ticks) {
                    book.best_bid_ticks = ev.price_ticks;
                    book.bid_qty = ev.quantity;
                } else if (ev.price_ticks == book.best_bid_ticks) {
                    book.bid_qty += ev.quantity;
                } else if (book.best_bid_ticks == 0) {
                    book.best_bid_ticks = ev.price_ticks;
                    book.bid_qty = ev.quantity;
                }
            } else {
                if (book.best_ask_ticks == 0 || ev.price_ticks < book.best_ask_ticks) {
                    book.best_ask_ticks = ev.price_ticks;
                    book.ask_qty = ev.quantity;
                } else if (ev.price_ticks == book.best_ask_ticks) {
                    book.ask_qty += ev.quantity;
                }
            }
            break;

        case EventType::kModify:
            ++modify_events_;
            if (side == Side::kBid) {
                book.best_bid_ticks = ev.price_ticks;
                book.bid_qty = ev.quantity;
            } else {
                book.best_ask_ticks = ev.price_ticks;
                book.ask_qty = ev.quantity;
            }
            break;

        case EventType::kCancel:
            ++cancel_events_;
            if (side == Side::kBid) {
                if (book.bid_qty == 0) {
                    status = 1;
                    accepted_qty = 0;
                } else {
                    const std::uint64_t dec = ev.quantity == 0
                        ? book.bid_qty
                        : std::min<std::uint64_t>(book.bid_qty, ev.quantity);
                    book.bid_qty -= dec;
                    accepted_qty = static_cast<std::uint32_t>(dec);
                    if (book.bid_qty == 0) {
                        book.best_bid_ticks = 0;
                    }
                }
            } else {
                if (book.ask_qty == 0) {
                    status = 1;
                    accepted_qty = 0;
                } else {
                    const std::uint64_t dec = ev.quantity == 0
                        ? book.ask_qty
                        : std::min<std::uint64_t>(book.ask_qty, ev.quantity);
                    book.ask_qty -= dec;
                    accepted_qty = static_cast<std::uint32_t>(dec);
                    if (book.ask_qty == 0) {
                        book.best_ask_ticks = 0;
                    }
                }
            }
            break;

        case EventType::kTrade:
            ++trade_events_;
            book.last_trade_ticks = ev.price_ticks;
            book.cumulative_volume += ev.quantity;
            book.cumulative_notional_ticks +=
                static_cast<std::int64_t>(ev.quantity) * ev.price_ticks;
            total_volume_ += ev.quantity;
            total_notional_ticks_ +=
                static_cast<std::int64_t>(ev.quantity) * ev.price_ticks;

            // Reduce passive liquidity on the opposite side if present
            if (side == Side::kBid && book.ask_qty > 0) {
                const std::uint64_t dec = std::min<std::uint64_t>(book.ask_qty, ev.quantity);
                book.ask_qty -= dec;
                if (book.ask_qty == 0) book.best_ask_ticks = 0;
            } else if (side == Side::kAsk && book.bid_qty > 0) {
                const std::uint64_t dec = std::min<std::uint64_t>(book.bid_qty, ev.quantity);
                book.bid_qty -= dec;
                if (book.bid_qty == 0) book.best_bid_ticks = 0;
            }
            break;
    }

    if (status == 0) {
        ++applied_events_;
    } else {
        ++noop_events_;
    }

    // Deterministic rolling digest of domain state transition (excludes wall-clock timestamps!)
    fold_digest(ev.seq_id);
    fold_digest((static_cast<std::uint64_t>(sym) << 32) |
                (static_cast<std::uint64_t>(ev.event_type) << 16) |
                (static_cast<std::uint64_t>(ev.side) << 8) |
                static_cast<std::uint64_t>(status));
    fold_digest(static_cast<std::uint64_t>(book.best_bid_ticks));
    fold_digest(book.bid_qty);
    fold_digest(static_cast<std::uint64_t>(book.best_ask_ticks));
    fold_digest(book.ask_qty);
    fold_digest(book.cumulative_volume);

    ProcessedEvent out{};
    out.seq_id = ev.seq_id;
    out.send_ts_ns = ev.send_ts_ns;
    out.recv_ts_ns = recv_ts_ns;
    out.done_ts_ns = done_ts_ns;
    out.best_bid_ticks = book.best_bid_ticks;
    out.best_ask_ticks = book.best_ask_ticks;
    out.accepted_qty = accepted_qty;
    out.symbol_id = sym;
    out.status = status;
    out.side = ev.side;
    out.state_digest = state_digest_;
    return out;
}

EngineSummary MatchingEngine::summary() const noexcept {
    return EngineSummary{
        total_events_,
        applied_events_,
        noop_events_,
        add_events_,
        modify_events_,
        cancel_events_,
        trade_events_,
        last_seq_id_,
        total_volume_,
        total_notional_ticks_,
        state_digest_,
        sequence_strictly_monotonic_};
}

std::string EngineSummary::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"total_events\":" << total_events << ","
        << "\"applied_events\":" << applied_events << ","
        << "\"noop_events\":" << noop_events << ","
        << "\"add_events\":" << add_events << ","
        << "\"modify_events\":" << modify_events << ","
        << "\"cancel_events\":" << cancel_events << ","
        << "\"trade_events\":" << trade_events << ","
        << "\"last_seq_id\":" << last_seq_id << ","
        << "\"total_volume\":" << total_volume << ","
        << "\"total_notional_ticks\":" << total_notional_ticks << ","
        << "\"final_state_digest\":\"" << std::hex << final_state_digest << std::dec << "\","
        << "\"sequence_strictly_monotonic\":" << (sequence_strictly_monotonic ? "true" : "false")
        << "}";
    return oss.str();
}

}  // namespace numa_engine
