#include "sor/consolidated_book.hpp"

#include <algorithm>
#include <limits>

#include "sor/venue_health.hpp"

namespace sor {

void VenueBook::clear() noexcept {
    bid_count = 0;
    ask_count = 0;
    has_data = false;
}

void VenueBook::set_snapshot(Side side, const PriceLevel* levels, std::size_t count) noexcept {
    auto& arr = (side == Side::Buy) ? bids : asks;
    auto& len = (side == Side::Buy) ? bid_count : ask_count;

    std::size_t n = 0;
    for (std::size_t i = 0; i < count && n < kMaxBookDepth; ++i) {
        if (levels[i].price > 0 && levels[i].quantity > 0) {
            arr[n++] = levels[i];
        }
    }

    if (side == Side::Buy) {
        std::sort(arr.begin(), arr.begin() + static_cast<std::ptrdiff_t>(n),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    } else {
        std::sort(arr.begin(), arr.begin() + static_cast<std::ptrdiff_t>(n),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });
    }

    // Merge duplicate prices if any appeared in snapshot.
    std::size_t write = 0;
    for (std::size_t read = 0; read < n; ++read) {
        if (write > 0 && arr[write - 1].price == arr[read].price) {
            arr[write - 1].quantity += arr[read].quantity;
            arr[write - 1].order_count += arr[read].order_count;
        } else {
            arr[write++] = arr[read];
        }
    }
    len = write;
    has_data = (bid_count > 0 || ask_count > 0);
}

void VenueBook::upsert_level(Side side, const PriceLevel& level) noexcept {
    if (level.price <= 0) return;

    auto& arr = (side == Side::Buy) ? bids : asks;
    auto& len = (side == Side::Buy) ? bid_count : ask_count;
    const bool is_bid = (side == Side::Buy);

    // Find existing price or insertion position.
    std::size_t pos = 0;
    while (pos < len) {
        if (arr[pos].price == level.price) {
            if (level.quantity <= 0) {
                // Delete level by shifting remaining elements left.
                for (std::size_t j = pos + 1; j < len; ++j) {
                    arr[j - 1] = arr[j];
                }
                --len;
            } else {
                arr[pos] = level;
            }
            has_data = (bid_count > 0 || ask_count > 0);
            return;
        }
        const bool goes_before = is_bid ? (level.price > arr[pos].price)
                                        : (level.price < arr[pos].price);
        if (goes_before) break;
        ++pos;
    }

    if (level.quantity <= 0 || pos >= kMaxBookDepth) {
        return;
    }

    const std::size_t new_len = (len < kMaxBookDepth) ? (len + 1) : kMaxBookDepth;
    for (std::size_t j = new_len - 1; j > pos; --j) {
        arr[j] = arr[j - 1];
    }
    arr[pos] = level;
    len = new_len;
    has_data = true;
}

void ConsolidatedBook::configure_venues(const std::vector<VenueConfig>& venues) {
    configured_.fill(false);
    venue_count_ = 0;
    for (std::size_t i = 0; i < kMaxVenues; ++i) {
        books_[i].clear();
        books_[i].venue_id = static_cast<VenueId>(i);
    }
    for (const auto& v : venues) {
        configure_venue(v);
    }
}

void ConsolidatedBook::configure_venue(const VenueConfig& venue) {
    if (venue.id >= kMaxVenues) return;
    if (!configured_[venue.id]) {
        ++venue_count_;
    }
    configs_[venue.id] = venue;
    configured_[venue.id] = true;
    books_[venue.id].venue_id = venue.id;
}

bool ConsolidatedBook::apply_update(const NormalizedQuoteUpdate& update) noexcept {
    if (update.venue_id >= kMaxVenues || !configured_[update.venue_id]) {
        return false;
    }
    auto& vb = books_[update.venue_id];
    vb.last_exchange_ts_ns = update.exchange_ts_ns;
    vb.last_recv_ts_ns = update.recv_ts_ns;
    if (update.seq > 0) {
        vb.last_seq = update.seq;
    }

    switch (update.type) {
        case QuoteUpdateType::ClearBook:
            vb.clear();
            break;
        case QuoteUpdateType::Snapshot:
            vb.set_snapshot(update.side, update.levels.data(), update.level_count);
            break;
        case QuoteUpdateType::UpsertLevel:
            for (std::uint8_t i = 0; i < update.level_count; ++i) {
                vb.upsert_level(update.side, update.levels[i]);
            }
            break;
    }
    return true;
}

bool ConsolidatedBook::set_level(VenueId venue_id, Side side, PriceTicks price, Quantity qty,
                                 TimestampNs exchange_ts_ns, TimestampNs recv_ts_ns,
                                 std::uint32_t order_count) noexcept {
    NormalizedQuoteUpdate u{};
    u.venue_id = venue_id;
    u.exchange_ts_ns = exchange_ts_ns;
    u.recv_ts_ns = recv_ts_ns;
    u.type = QuoteUpdateType::UpsertLevel;
    u.side = side;
    u.level_count = 1;
    u.levels[0] = PriceLevel{price, qty, order_count};
    return apply_update(u);
}

void ConsolidatedBook::deplete_liquidity(VenueId venue_id, Side book_side, PriceTicks price,
                                         Quantity qty_removed) noexcept {
    if (venue_id >= kMaxVenues || !configured_[venue_id] || qty_removed <= 0) return;
    auto& vb = books_[venue_id];
    auto& arr = (book_side == Side::Buy) ? vb.bids : vb.asks;
    auto& len = (book_side == Side::Buy) ? vb.bid_count : vb.ask_count;
    for (std::size_t i = 0; i < len; ++i) {
        if (arr[i].price == price) {
            if (arr[i].quantity <= qty_removed) {
                for (std::size_t j = i + 1; j < len; ++j) {
                    arr[j - 1] = arr[j];
                }
                --len;
            } else {
                arr[i].quantity -= qty_removed;
            }
            vb.has_data = (vb.bid_count > 0 || vb.ask_count > 0);
            return;
        }
    }
}

TimestampNs ConsolidatedBook::quote_age_ns(VenueId id, TimestampNs now_ns) const noexcept {
    if (id >= kMaxVenues || !configured_[id] || !books_[id].has_data) {
        return std::numeric_limits<TimestampNs>::max();
    }
    const TimestampNs delta = now_ns - books_[id].last_recv_ts_ns;
    return delta > 0 ? delta : 0;
}

bool ConsolidatedBook::is_venue_stale(VenueId id, TimestampNs now_ns) const noexcept {
    if (id >= kMaxVenues || !configured_[id] || !books_[id].has_data) {
        return true;
    }
    const TimestampNs timeout = configs_[id].stale_timeout_ns;
    if (timeout <= 0) return false;
    return (now_ns - books_[id].last_recv_ts_ns) > timeout;
}

ConsolidatedBBO ConsolidatedBook::consolidated_bbo(TimestampNs now_ns,
                                                   const VenueHealthMonitor* health,
                                                   bool respect_staleness,
                                                   bool respect_health) const noexcept {
    ConsolidatedBBO bbo{};
    bbo.best_eff_bid_ticks = -std::numeric_limits<double>::infinity();
    bbo.best_eff_ask_ticks = std::numeric_limits<double>::infinity();

    for (VenueId vid = 0; vid < kMaxVenues; ++vid) {
        if (!configured_[vid]) continue;
        const auto& cfg = configs_[vid];
        if (!cfg.enabled) continue;
        const auto& vb = books_[vid];
        if (!vb.has_data) continue;
        if (respect_health && health != nullptr && !health->is_routable(vid, now_ns)) {
            continue;
        }
        if (respect_staleness && is_venue_stale(vid, now_ns)) {
            continue;
        }

        ++bbo.eligible_venues;

        if (vb.has_bid()) {
            const auto& top = vb.best_bid();
            if (!bbo.has_bid || top.price > bbo.best_bid_price) {
                bbo.has_bid = true;
                bbo.best_bid_price = top.price;
                bbo.best_bid_qty = top.quantity;
                bbo.best_bid_venue = vid;
            } else if (top.price == bbo.best_bid_price) {
                bbo.best_bid_qty += top.quantity;
            }

            // For a SELL hitting the bid, effective proceeds = raw_bid - taker_fee.
            const double eff_bid = cfg.effective_price_ticks(Side::Sell, top.price);
            if (eff_bid > bbo.best_eff_bid_ticks) {
                bbo.best_eff_bid_ticks = eff_bid;
                bbo.best_eff_bid_venue = vid;
            }
        }

        if (vb.has_ask()) {
            const auto& top = vb.best_ask();
            if (!bbo.has_ask || top.price < bbo.best_ask_price) {
                bbo.has_ask = true;
                bbo.best_ask_price = top.price;
                bbo.best_ask_qty = top.quantity;
                bbo.best_ask_venue = vid;
            } else if (top.price == bbo.best_ask_price) {
                bbo.best_ask_qty += top.quantity;
            }

            // For a BUY lifting the ask, effective cost = raw_ask + taker_fee.
            const double eff_ask = cfg.effective_price_ticks(Side::Buy, top.price);
            if (eff_ask < bbo.best_eff_ask_ticks) {
                bbo.best_eff_ask_ticks = eff_ask;
                bbo.best_eff_ask_venue = vid;
            }
        }
    }

    if (!bbo.has_bid) bbo.best_eff_bid_ticks = 0.0;
    if (!bbo.has_ask) bbo.best_eff_ask_ticks = 0.0;
    if (bbo.has_bid && bbo.has_ask && bbo.best_bid_price >= bbo.best_ask_price) {
        bbo.crossed = true;
    }
    return bbo;
}

void ConsolidatedBook::clear_all() noexcept {
    for (auto& b : books_) {
        b.clear();
    }
}

}  // namespace sor
