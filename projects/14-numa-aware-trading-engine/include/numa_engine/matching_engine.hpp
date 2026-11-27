#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "numa_engine/types.hpp"

namespace numa_engine {

struct SymbolBookState {
    std::int64_t best_bid_ticks = 0;
    std::uint64_t bid_qty = 0;
    std::int64_t best_ask_ticks = 0;
    std::uint64_t ask_qty = 0;
    std::int64_t last_trade_ticks = 0;
    std::uint64_t cumulative_volume = 0;
    std::int64_t cumulative_notional_ticks = 0;
    std::uint64_t event_count = 0;
};

struct EngineSummary {
    std::uint64_t total_events = 0;
    std::uint64_t applied_events = 0;
    std::uint64_t noop_events = 0;
    std::uint64_t add_events = 0;
    std::uint64_t modify_events = 0;
    std::uint64_t cancel_events = 0;
    std::uint64_t trade_events = 0;
    std::uint64_t last_seq_id = 0;
    std::uint64_t total_volume = 0;
    std::int64_t total_notional_ticks = 0;
    std::uint64_t final_state_digest = 0;
    bool sequence_strictly_monotonic = true;

    [[nodiscard]] bool operator==(const EngineSummary& other) const noexcept = default;
    [[nodiscard]] std::string to_json() const;
};

// Pure, deterministic domain state machine isolated from all I/O, threading,
// and memory-placement code. Identical event streams MUST produce identical
// EngineSummary and per-symbol book states regardless of pipeline or NUMA placement.
class MatchingEngine {
public:
    MatchingEngine() noexcept;

    void reset() noexcept;

    [[nodiscard]] ProcessedEvent process(
        const MarketEvent& ev,
        std::uint64_t recv_ts_ns = 0,
        std::uint64_t done_ts_ns = 0) noexcept;

    [[nodiscard]] const SymbolBookState& book(std::uint16_t symbol_id) const noexcept {
        return books_[symbol_id % kMaxSymbols];
    }

    [[nodiscard]] EngineSummary summary() const noexcept;

private:
    void fold_digest(std::uint64_t value) noexcept;

    std::array<SymbolBookState, kMaxSymbols> books_{};
    std::uint64_t total_events_ = 0;
    std::uint64_t applied_events_ = 0;
    std::uint64_t noop_events_ = 0;
    std::uint64_t add_events_ = 0;
    std::uint64_t modify_events_ = 0;
    std::uint64_t cancel_events_ = 0;
    std::uint64_t trade_events_ = 0;
    std::uint64_t last_seq_id_ = 0;
    std::uint64_t total_volume_ = 0;
    std::int64_t total_notional_ticks_ = 0;
    std::uint64_t state_digest_ = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
    bool sequence_strictly_monotonic_ = true;
};

}  // namespace numa_engine
