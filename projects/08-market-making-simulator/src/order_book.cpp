#include "mmsim/order_book.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace mmsim {

namespace {

size_t round_up_64(size_t n) noexcept {
    n = std::max<size_t>(n, 64);
    return ((n + 63) / 64) * 64;
}

} // namespace

// ============================================================================
// FlatArrayBook (2-level 64-bit hardware bitset accelerated)
// ============================================================================

FlatArrayBook::FlatArrayBook(size_t window_ticks)
    : window_size_(round_up_64(window_ticks)),
      num_words_(window_size_ / 64),
      bids_(window_size_, 0),
      asks_(window_size_, 0),
      bid_words_(num_words_, 0),
      ask_words_(num_words_, 0),
      bid_summary_((num_words_ + 63) / 64, 0),
      ask_summary_((num_words_ + 63) / 64, 0) {}

void FlatArrayBook::clear() {
    std::fill(bids_.begin(), bids_.end(), 0);
    std::fill(asks_.begin(), asks_.end(), 0);
    std::fill(bid_words_.begin(), bid_words_.end(), 0);
    std::fill(ask_words_.begin(), ask_words_.end(), 0);
    std::fill(bid_summary_.begin(), bid_summary_.end(), 0);
    std::fill(ask_summary_.begin(), ask_summary_.end(), 0);
    anchored_ = false;
    base_price_ = 0;
    best_bid_idx_ = -1;
    best_ask_idx_ = -1;
}

void FlatArrayBook::set_bit(Side side, size_t idx) noexcept {
    const size_t w = idx >> 6;
    const size_t b = idx & 63u;
    const size_t sw = w >> 6;
    const size_t sb = w & 63u;
    if (side == Side::Bid) {
        bid_words_[w] |= (uint64_t{1} << b);
        bid_summary_[sw] |= (uint64_t{1} << sb);
    } else {
        ask_words_[w] |= (uint64_t{1} << b);
        ask_summary_[sw] |= (uint64_t{1} << sb);
    }
}

void FlatArrayBook::clear_bit(Side side, size_t idx) noexcept {
    const size_t w = idx >> 6;
    const size_t b = idx & 63u;
    const size_t sw = w >> 6;
    const size_t sb = w & 63u;
    if (side == Side::Bid) {
        bid_words_[w] &= ~(uint64_t{1} << b);
        if (bid_words_[w] == 0) {
            bid_summary_[sw] &= ~(uint64_t{1} << sb);
        }
    } else {
        ask_words_[w] &= ~(uint64_t{1} << b);
        if (ask_words_[w] == 0) {
            ask_summary_[sw] &= ~(uint64_t{1} << sb);
        }
    }
}

int FlatArrayBook::find_highest_bid_below(int start_idx) const noexcept {
    if (start_idx < 0) {
        return -1;
    }
    const auto idx = static_cast<size_t>(start_idx);
    size_t w = idx >> 6;
    const unsigned bit = static_cast<unsigned>(idx & 63u);

    // Check remaining bits in current word (0..bit inclusive).
    const uint64_t mask = (bit == 63u) ? ~uint64_t{0} : ((uint64_t{1} << (bit + 1u)) - 1u);
    uint64_t cur = bid_words_[w] & mask;
    if (cur != 0) {
        const int hi = 63 - std::countl_zero(cur);
        return static_cast<int>((w << 6) + static_cast<size_t>(hi));
    }

    if (w == 0) {
        return -1;
    }
    --w;
    size_t sw = w >> 6;
    const unsigned sb = static_cast<unsigned>(w & 63u);
    const uint64_t smask = (sb == 63u) ? ~uint64_t{0} : ((uint64_t{1} << (sb + 1u)) - 1u);

    uint64_t s = bid_summary_[sw] & smask;
    while (s == 0) {
        if (sw == 0) {
            return -1;
        }
        --sw;
        s = bid_summary_[sw];
    }
    const size_t target_w = (sw << 6) + static_cast<size_t>(63 - std::countl_zero(s));
    const int hi = 63 - std::countl_zero(bid_words_[target_w]);
    return static_cast<int>((target_w << 6) + static_cast<size_t>(hi));
}

int FlatArrayBook::find_lowest_ask_above(int start_idx) const noexcept {
    if (start_idx < 0 || static_cast<size_t>(start_idx) >= window_size_) {
        return -1;
    }
    const auto idx = static_cast<size_t>(start_idx);
    size_t w = idx >> 6;
    const unsigned bit = static_cast<unsigned>(idx & 63u);

    // Check remaining bits in current word (bit..63 inclusive).
    const uint64_t mask = ~uint64_t{0} << bit;
    uint64_t cur = ask_words_[w] & mask;
    if (cur != 0) {
        const int lo = std::countr_zero(cur);
        return static_cast<int>((w << 6) + static_cast<size_t>(lo));
    }

    ++w;
    if (w >= num_words_) {
        return -1;
    }
    size_t sw = w >> 6;
    const unsigned sb = static_cast<unsigned>(w & 63u);
    const uint64_t smask = ~uint64_t{0} << sb;

    uint64_t s = ask_summary_[sw] & smask;
    while (s == 0) {
        ++sw;
        if (sw >= ask_summary_.size()) {
            return -1;
        }
        s = ask_summary_[sw];
    }
    const size_t target_w = (sw << 6) + static_cast<size_t>(std::countr_zero(s));
    const int lo = std::countr_zero(ask_words_[target_w]);
    return static_cast<int>((target_w << 6) + static_cast<size_t>(lo));
}

void FlatArrayBook::init_anchor(PriceTicks anchor_price) {
    base_price_ = anchor_price - static_cast<PriceTicks>(window_size_ / 2);
    anchored_ = true;
}

void FlatArrayBook::recenter(PriceTicks target_price) {
    PriceTicks min_p = target_price;
    PriceTicks max_p = target_price;
    for (size_t w = 0; w < num_words_; ++w) {
        uint64_t bits = bid_words_[w] | ask_words_[w];
        while (bits != 0) {
            const int b = std::countr_zero(bits);
            const PriceTicks p = base_price_ + static_cast<PriceTicks>((w << 6) + static_cast<size_t>(b));
            min_p = std::min(min_p, p);
            max_p = std::max(max_p, p);
            bits &= (bits - 1u);
        }
    }

    const int64_t span = max_p - min_p + 1;
    size_t new_window = window_size_;
    while (static_cast<int64_t>(new_window) < span + 64) {
        new_window *= 2;
    }
    new_window = round_up_64(new_window);
    const size_t new_words = new_window / 64;
    const PriceTicks center = min_p + (max_p - min_p) / 2;
    const PriceTicks new_base = center - static_cast<PriceTicks>(new_window / 2);

    std::vector<Qty> new_bids(new_window, 0);
    std::vector<Qty> new_asks(new_window, 0);
    std::vector<uint64_t> new_bid_words(new_words, 0);
    std::vector<uint64_t> new_ask_words(new_words, 0);
    std::vector<uint64_t> new_bid_summary((new_words + 63) / 64, 0);
    std::vector<uint64_t> new_ask_summary((new_words + 63) / 64, 0);

    for (size_t w = 0; w < num_words_; ++w) {
        uint64_t bb = bid_words_[w];
        while (bb != 0) {
            const int b = std::countr_zero(bb);
            const size_t old_idx = (w << 6) + static_cast<size_t>(b);
            const PriceTicks p = base_price_ + static_cast<PriceTicks>(old_idx);
            const auto ni = static_cast<size_t>(p - new_base);
            new_bids[ni] = bids_[old_idx];
            new_bid_words[ni >> 6] |= (uint64_t{1} << (ni & 63u));
            new_bid_summary[ni >> 12] |= (uint64_t{1} << ((ni >> 6) & 63u));
            bb &= (bb - 1u);
        }
        uint64_t ab = ask_words_[w];
        while (ab != 0) {
            const int b = std::countr_zero(ab);
            const size_t old_idx = (w << 6) + static_cast<size_t>(b);
            const PriceTicks p = base_price_ + static_cast<PriceTicks>(old_idx);
            const auto ni = static_cast<size_t>(p - new_base);
            new_asks[ni] = asks_[old_idx];
            new_ask_words[ni >> 6] |= (uint64_t{1} << (ni & 63u));
            new_ask_summary[ni >> 12] |= (uint64_t{1} << ((ni >> 6) & 63u));
            ab &= (ab - 1u);
        }
    }

    bids_.swap(new_bids);
    asks_.swap(new_asks);
    bid_words_.swap(new_bid_words);
    ask_words_.swap(new_ask_words);
    bid_summary_.swap(new_bid_summary);
    ask_summary_.swap(new_ask_summary);
    window_size_ = new_window;
    num_words_ = new_words;
    base_price_ = new_base;
    best_bid_idx_ = find_highest_bid_below(static_cast<int>(window_size_) - 1);
    best_ask_idx_ = find_lowest_ask_above(0);
}

Qty FlatArrayBook::apply_delta(Side side, PriceTicks price_ticks, Qty new_qty) {
    if (new_qty < 0) {
        new_qty = 0;
    }
    if (!anchored_) {
        if (new_qty == 0) {
            return 0;
        }
        init_anchor(price_ticks);
    }

    int64_t rel = price_ticks - base_price_;
    if (rel < 0 || rel >= static_cast<int64_t>(window_size_)) {
        if (new_qty == 0) {
            return 0;
        }
        recenter(price_ticks);
        rel = price_ticks - base_price_;
    }

    const auto idx = static_cast<size_t>(rel);
    const int iidx = static_cast<int>(rel);
    if (side == Side::Bid) {
        const Qty old_qty = bids_[idx];
        bids_[idx] = new_qty;
        if (new_qty > 0) {
            if (old_qty == 0) {
                set_bit(Side::Bid, idx);
            }
            if (best_bid_idx_ < 0 || iidx > best_bid_idx_) {
                best_bid_idx_ = iidx;
            }
        } else if (old_qty > 0) {
            clear_bit(Side::Bid, idx);
            if (iidx == best_bid_idx_) {
                best_bid_idx_ = find_highest_bid_below(iidx - 1);
            }
        }
        return old_qty;
    } else {
        const Qty old_qty = asks_[idx];
        asks_[idx] = new_qty;
        if (new_qty > 0) {
            if (old_qty == 0) {
                set_bit(Side::Ask, idx);
            }
            if (best_ask_idx_ < 0 || iidx < best_ask_idx_) {
                best_ask_idx_ = iidx;
            }
        } else if (old_qty > 0) {
            clear_bit(Side::Ask, idx);
            if (iidx == best_ask_idx_) {
                best_ask_idx_ = find_lowest_ask_above(iidx + 1);
            }
        }
        return old_qty;
    }
}

TopOfBook FlatArrayBook::top_of_book() const noexcept {
    TopOfBook tob{};
    if (!anchored_) {
        return tob;
    }
    if (best_bid_idx_ >= 0) {
        tob.best_bid = base_price_ + static_cast<PriceTicks>(best_bid_idx_);
        tob.bid_qty = bids_[static_cast<size_t>(best_bid_idx_)];
    }
    if (best_ask_idx_ >= 0) {
        tob.best_ask = base_price_ + static_cast<PriceTicks>(best_ask_idx_);
        tob.ask_qty = asks_[static_cast<size_t>(best_ask_idx_)];
    }
    return tob;
}

Qty FlatArrayBook::level_qty(Side side, PriceTicks price_ticks) const noexcept {
    if (!anchored_) {
        return 0;
    }
    const int64_t rel = price_ticks - base_price_;
    if (rel < 0 || rel >= static_cast<int64_t>(window_size_)) {
        return 0;
    }
    const auto idx = static_cast<size_t>(rel);
    return side == Side::Bid ? bids_[idx] : asks_[idx];
}

size_t FlatArrayBook::top_levels(Side side, size_t max_levels,
                                 std::array<PriceLevel, kMaxFeatureDepth>& out) const noexcept {
    if (!anchored_) {
        return 0;
    }
    const size_t cap = std::min(max_levels, kMaxFeatureDepth);
    size_t count = 0;
    if (side == Side::Bid) {
        int cur = best_bid_idx_;
        while (cur >= 0 && count < cap) {
            const auto u = static_cast<size_t>(cur);
            out[count++] = PriceLevel{base_price_ + static_cast<PriceTicks>(cur), bids_[u]};
            cur = find_highest_bid_below(cur - 1);
        }
    } else {
        int cur = best_ask_idx_;
        while (cur >= 0 && count < cap) {
            const auto u = static_cast<size_t>(cur);
            out[count++] = PriceLevel{base_price_ + static_cast<PriceTicks>(cur), asks_[u]};
            cur = find_lowest_ask_above(cur + 1);
        }
    }
    return count;
}

// ============================================================================
// MapBook
// ============================================================================

void MapBook::clear() {
    bids_.clear();
    asks_.clear();
}

Qty MapBook::apply_delta(Side side, PriceTicks price_ticks, Qty new_qty) {
    if (new_qty < 0) {
        new_qty = 0;
    }
    if (side == Side::Bid) {
        auto it = bids_.find(price_ticks);
        const Qty old_qty = (it != bids_.end()) ? it->second : 0;
        if (new_qty == 0) {
            if (it != bids_.end()) {
                bids_.erase(it);
            }
        } else {
            bids_[price_ticks] = new_qty;
        }
        return old_qty;
    } else {
        auto it = asks_.find(price_ticks);
        const Qty old_qty = (it != asks_.end()) ? it->second : 0;
        if (new_qty == 0) {
            if (it != asks_.end()) {
                asks_.erase(it);
            }
        } else {
            asks_[price_ticks] = new_qty;
        }
        return old_qty;
    }
}

TopOfBook MapBook::top_of_book() const noexcept {
    TopOfBook tob{};
    if (!bids_.empty()) {
        tob.best_bid = bids_.begin()->first;
        tob.bid_qty = bids_.begin()->second;
    }
    if (!asks_.empty()) {
        tob.best_ask = asks_.begin()->first;
        tob.ask_qty = asks_.begin()->second;
    }
    return tob;
}

Qty MapBook::level_qty(Side side, PriceTicks price_ticks) const noexcept {
    if (side == Side::Bid) {
        const auto it = bids_.find(price_ticks);
        return it != bids_.end() ? it->second : 0;
    }
    const auto it = asks_.find(price_ticks);
    return it != asks_.end() ? it->second : 0;
}

size_t MapBook::top_levels(Side side, size_t max_levels,
                           std::array<PriceLevel, kMaxFeatureDepth>& out) const noexcept {
    const size_t cap = std::min(max_levels, kMaxFeatureDepth);
    size_t count = 0;
    if (side == Side::Bid) {
        for (const auto& [p, q] : bids_) {
            if (count >= cap) {
                break;
            }
            out[count++] = PriceLevel{p, q};
        }
    } else {
        for (const auto& [p, q] : asks_) {
            if (count >= cap) {
                break;
            }
            out[count++] = PriceLevel{p, q};
        }
    }
    return count;
}

std::unique_ptr<IL2Book> make_book(BookImplType impl) {
    switch (impl) {
    case BookImplType::FlatArray: return std::make_unique<FlatArrayBook>();
    case BookImplType::StdMap: return std::make_unique<MapBook>();
    }
    return std::make_unique<FlatArrayBook>();
}

} // namespace mmsim
