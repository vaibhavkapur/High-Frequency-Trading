#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sor/quote_normalizer.hpp"
#include "sor/types.hpp"

namespace sor {

class VenueHealthMonitor;

// Single-venue L2 order book with fixed-capacity sorted bid/ask arrays (zero heap allocation).
struct VenueBook {
    VenueId venue_id{kInvalidVenue};
    bool has_data{false};
    TimestampNs last_exchange_ts_ns{0};
    TimestampNs last_recv_ts_ns{0};
    std::uint64_t last_seq{0};

    std::size_t bid_count{0};
    std::size_t ask_count{0};
    std::array<PriceLevel, kMaxBookDepth> bids{};  // Sorted descending by price
    std::array<PriceLevel, kMaxBookDepth> asks{};  // Sorted ascending by price

    void clear() noexcept;
    void set_snapshot(Side side, const PriceLevel* levels, std::size_t count) noexcept;
    void upsert_level(Side side, const PriceLevel& level) noexcept;

    [[nodiscard]] bool has_bid() const noexcept { return bid_count > 0; }
    [[nodiscard]] bool has_ask() const noexcept { return ask_count > 0; }
    [[nodiscard]] const PriceLevel& best_bid() const noexcept { return bids[0]; }
    [[nodiscard]] const PriceLevel& best_ask() const noexcept { return asks[0]; }
};

// Simulated consolidated top-of-book across eligible venues.
// NOTE: Educational consolidated view only — not a regulatory NBBO feed.
struct ConsolidatedBBO {
    PriceTicks best_bid_price{kInvalidPrice};
    Quantity best_bid_qty{0};
    VenueId best_bid_venue{kInvalidVenue};

    PriceTicks best_ask_price{kInvalidPrice};
    Quantity best_ask_qty{0};
    VenueId best_ask_venue{kInvalidVenue};

    // Fee-adjusted effective top-of-book.
    double best_eff_bid_ticks{0.0};
    VenueId best_eff_bid_venue{kInvalidVenue};
    double best_eff_ask_ticks{0.0};
    VenueId best_eff_ask_venue{kInvalidVenue};

    bool has_bid{false};
    bool has_ask{false};
    bool crossed{false};
    std::uint8_t eligible_venues{0};

    [[nodiscard]] double mid_price_ticks() const noexcept {
        if (has_bid && has_ask) {
            return 0.5 * static_cast<double>(best_bid_price + best_ask_price);
        }
        if (has_ask) return static_cast<double>(best_ask_price);
        if (has_bid) return static_cast<double>(best_bid_price);
        return 0.0;
    }

    [[nodiscard]] PriceTicks spread_ticks() const noexcept {
        if (!has_bid || !has_ask) return 0;
        return best_ask_price - best_bid_price;
    }
};

class ConsolidatedBook {
public:
    ConsolidatedBook() = default;

    void configure_venues(const std::vector<VenueConfig>& venues);
    void configure_venue(const VenueConfig& venue);

    // Apply a normalized quote update to the target venue's L2 book.
    bool apply_update(const NormalizedQuoteUpdate& update) noexcept;

    // Convenience helper to directly set a single level for a venue.
    bool set_level(VenueId venue_id, Side side, PriceTicks price, Quantity qty,
                   TimestampNs exchange_ts_ns, TimestampNs recv_ts_ns,
                   std::uint32_t order_count = 1) noexcept;

    // Consume displayed liquidity locally (e.g., after an execution or synthetic depletion).
    void deplete_liquidity(VenueId venue_id, Side book_side, PriceTicks price,
                           Quantity qty_removed) noexcept;

    [[nodiscard]] std::size_t venue_count() const noexcept { return venue_count_; }
    [[nodiscard]] bool has_venue(VenueId id) const noexcept {
        return id < kMaxVenues && configured_[id];
    }
    [[nodiscard]] const VenueConfig& venue_config(VenueId id) const noexcept {
        return configs_[id];
    }
    [[nodiscard]] VenueConfig& mutable_venue_config(VenueId id) noexcept { return configs_[id]; }
    [[nodiscard]] const VenueBook& venue_book(VenueId id) const noexcept { return books_[id]; }

    [[nodiscard]] TimestampNs quote_age_ns(VenueId id, TimestampNs now_ns) const noexcept;
    [[nodiscard]] bool is_venue_stale(VenueId id, TimestampNs now_ns) const noexcept;

    // Compute consolidated BBO (raw and fee-adjusted) across eligible venues.
    [[nodiscard]] ConsolidatedBBO consolidated_bbo(
        TimestampNs now_ns, const VenueHealthMonitor* health = nullptr,
        bool respect_staleness = true, bool respect_health = true) const noexcept;

    void clear_all() noexcept;

private:
    std::array<VenueConfig, kMaxVenues> configs_{};
    std::array<bool, kMaxVenues> configured_{};
    std::array<VenueBook, kMaxVenues> books_{};
    std::size_t venue_count_{0};
};

}  // namespace sor
