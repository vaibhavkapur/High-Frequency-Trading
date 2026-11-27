#include "mmae/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mmae {

namespace {

size_t next_power_of_two(size_t n) {
    size_t p = 16;
    while (p < n) {
        p <<= 1U;
    }
    return p;
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

}  // namespace

// ============================================================================
// BaselineOrderBook (std::map + std::list + std::unordered_map)
// ============================================================================

BaselineOrderBook::BaselineOrderBook(size_t max_levels, size_t max_orders)
    : max_levels_(max_levels), max_orders_(max_orders) {}

void BaselineOrderBook::clear(Side side) {
    if (side == Side::UNKNOWN) {
        bids_.clear();
        asks_.clear();
        orders_.clear();
        return;
    }
    if (side == Side::BID) {
        for (auto it = orders_.begin(); it != orders_.end();) {
            if (it->second.side == Side::BID) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
        bids_.clear();
    } else if (side == Side::ASK) {
        for (auto it = orders_.begin(); it != orders_.end();) {
            if (it->second.side == Side::ASK) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
        asks_.clear();
    }
}

BookErrorCode BaselineOrderBook::set_l2_level(Side side, int64_t price_ticks, int64_t qty) {
    if (side == Side::UNKNOWN) return BookErrorCode::INVALID_SIDE;
    if (price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (qty < 0) return BookErrorCode::INVALID_QTY;

    auto apply = [&](auto& levels) -> BookErrorCode {
        auto it = levels.find(price_ticks);
        if (qty == 0) {
            if (it != levels.end()) {
                for (const auto& node : it->second.queue) {
                    orders_.erase(node.order_id);
                }
                levels.erase(it);
            }
            return BookErrorCode::OK;
        }
        if (it == levels.end()) {
            if (levels.size() >= max_levels_) {
                return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
            }
            Level lvl;
            lvl.total_qty = qty;
            levels.emplace(price_ticks, std::move(lvl));
            return BookErrorCode::OK;
        }
        for (const auto& node : it->second.queue) {
            orders_.erase(node.order_id);
        }
        it->second.queue.clear();
        it->second.total_qty = qty;
        return BookErrorCode::OK;
    };

    return (side == Side::BID) ? apply(bids_) : apply(asks_);
}

BookErrorCode BaselineOrderBook::add_l3_order(uint64_t order_id, Side side, int64_t price_ticks, int64_t qty) {
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (side == Side::UNKNOWN) return BookErrorCode::INVALID_SIDE;
    if (price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (qty <= 0) return BookErrorCode::INVALID_QTY;

    if (orders_.find(order_id) != orders_.end()) {
        return BookErrorCode::DUPLICATE_ORDER_ID;
    }
    if (orders_.size() >= max_orders_) {
        return BookErrorCode::CAPACITY_ORDERS_EXCEEDED;
    }

    auto apply = [&](auto& levels) -> BookErrorCode {
        auto it = levels.find(price_ticks);
        if (it == levels.end()) {
            if (levels.size() >= max_levels_) {
                return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
            }
            auto [inserted, _] = levels.emplace(price_ticks, Level{});
            it = inserted;
        }
        it->second.queue.push_back(OrderNode{order_id, qty});
        it->second.total_qty += qty;
        auto node_it = std::prev(it->second.queue.end());
        orders_.emplace(order_id, OrderRef{side, price_ticks, node_it});
        return BookErrorCode::OK;
    };

    return (side == Side::BID) ? apply(bids_) : apply(asks_);
}

BookErrorCode BaselineOrderBook::modify_l3_order(uint64_t order_id, int64_t new_price_ticks, int64_t new_qty) {
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (new_price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (new_qty < 0) return BookErrorCode::INVALID_QTY;

    auto oit = orders_.find(order_id);
    if (oit == orders_.end()) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = oit->second.side;
    int64_t old_price = oit->second.price_ticks;
    auto node_it = oit->second.it;
    int64_t old_qty = node_it->qty;

    auto apply = [&](auto& levels) -> BookErrorCode {
        auto old_lit = levels.find(old_price);
        if (old_lit == levels.end()) {
            return BookErrorCode::ORDER_NOT_FOUND;
        }

        if (new_qty == 0) {
            old_lit->second.total_qty -= old_qty;
            old_lit->second.queue.erase(node_it);
            if (old_lit->second.total_qty <= 0 && old_lit->second.queue.empty()) {
                levels.erase(old_lit);
            }
            orders_.erase(oit);
            return BookErrorCode::OK;
        }

        // Same price and quantity decrease (or unchanged): preserve FIFO priority.
        if (new_price_ticks == old_price && new_qty <= old_qty) {
            old_lit->second.total_qty += (new_qty - old_qty);
            node_it->qty = new_qty;
            return BookErrorCode::OK;
        }

        // Price change or quantity increase: check level capacity before mutating.
        if (new_price_ticks != old_price) {
            auto new_lit = levels.find(new_price_ticks);
            bool old_will_vanish = (old_lit->second.queue.size() == 1 && old_lit->second.total_qty == old_qty);
            if (new_lit == levels.end() && !old_will_vanish && levels.size() >= max_levels_) {
                return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
            }
        }

        // Remove from old level.
        old_lit->second.total_qty -= old_qty;
        old_lit->second.queue.erase(node_it);
        if (old_lit->second.total_qty <= 0 && old_lit->second.queue.empty()) {
            levels.erase(old_lit);
        }

        // Insert at tail of new level (loses FIFO priority).
        auto new_lit = levels.find(new_price_ticks);
        if (new_lit == levels.end()) {
            auto [inserted, _] = levels.emplace(new_price_ticks, Level{});
            new_lit = inserted;
        }
        new_lit->second.queue.push_back(OrderNode{order_id, new_qty});
        new_lit->second.total_qty += new_qty;
        oit->second.price_ticks = new_price_ticks;
        oit->second.it = std::prev(new_lit->second.queue.end());
        return BookErrorCode::OK;
    };

    return (side == Side::BID) ? apply(bids_) : apply(asks_);
}

BookErrorCode BaselineOrderBook::cancel_l3_order(uint64_t order_id, int64_t cancel_qty) {
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (cancel_qty < 0) return BookErrorCode::INVALID_QTY;

    auto oit = orders_.find(order_id);
    if (oit == orders_.end()) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = oit->second.side;
    int64_t price = oit->second.price_ticks;
    auto node_it = oit->second.it;

    auto apply = [&](auto& levels) -> BookErrorCode {
        auto lit = levels.find(price);
        if (lit == levels.end()) return BookErrorCode::ORDER_NOT_FOUND;

        int64_t remove_qty = (cancel_qty == 0 || cancel_qty >= node_it->qty) ? node_it->qty : cancel_qty;
        node_it->qty -= remove_qty;
        lit->second.total_qty -= remove_qty;

        if (node_it->qty <= 0) {
            lit->second.queue.erase(node_it);
            orders_.erase(oit);
            if (lit->second.total_qty <= 0 && lit->second.queue.empty()) {
                levels.erase(lit);
            }
        }
        return BookErrorCode::OK;
    };

    return (side == Side::BID) ? apply(bids_) : apply(asks_);
}

BookErrorCode BaselineOrderBook::execute_l3_order(uint64_t order_id, int64_t exec_qty, QueueSnapshot& pre_exec_queue) {
    pre_exec_queue = QueueSnapshot{};
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (exec_qty <= 0) return BookErrorCode::INVALID_QTY;

    auto oit = orders_.find(order_id);
    if (oit == orders_.end()) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = oit->second.side;
    int64_t price = oit->second.price_ticks;
    auto node_it = oit->second.it;

    auto apply = [&](auto& levels) -> BookErrorCode {
        auto lit = levels.find(price);
        if (lit == levels.end()) return BookErrorCode::ORDER_NOT_FOUND;

        int64_t ahead_qty = 0;
        size_t ahead_orders = 0;
        for (auto it = lit->second.queue.begin(); it != node_it; ++it) {
            ahead_qty += it->qty;
            ++ahead_orders;
        }

        pre_exec_queue.found = true;
        pre_exec_queue.side = side;
        pre_exec_queue.price_ticks = price;
        pre_exec_queue.queue_ahead_qty = ahead_qty;
        pre_exec_queue.queue_ahead_orders = ahead_orders;
        pre_exec_queue.level_total_qty = lit->second.total_qty;
        pre_exec_queue.queue_position_ratio =
            (lit->second.total_qty > 0)
                ? static_cast<double>(ahead_qty) / static_cast<double>(lit->second.total_qty)
                : 0.0;

        int64_t fill = std::min(exec_qty, node_it->qty);
        node_it->qty -= fill;
        lit->second.total_qty -= fill;
        if (node_it->qty <= 0) {
            lit->second.queue.erase(node_it);
            orders_.erase(oit);
            if (lit->second.total_qty <= 0 && lit->second.queue.empty()) {
                levels.erase(lit);
            }
        }
        return BookErrorCode::OK;
    };

    return (side == Side::BID) ? apply(bids_) : apply(asks_);
}

TopOfBook BaselineOrderBook::top_of_book(size_t depth_k) const {
    TopOfBook tob{};
    if (!bids_.empty()) {
        auto it = bids_.begin();
        tob.has_bid = true;
        tob.best_bid_ticks = it->first;
        tob.bid_qty_l1 = it->second.total_qty;
        tob.bid_l1_orders = it->second.queue.size();
        size_t count = 0;
        for (auto bit = bids_.begin(); bit != bids_.end() && count < depth_k; ++bit, ++count) {
            tob.bid_depth_k += bit->second.total_qty;
        }
    }
    if (!asks_.empty()) {
        auto it = asks_.begin();
        tob.has_ask = true;
        tob.best_ask_ticks = it->first;
        tob.ask_qty_l1 = it->second.total_qty;
        tob.ask_l1_orders = it->second.queue.size();
        size_t count = 0;
        for (auto ait = asks_.begin(); ait != asks_.end() && count < depth_k; ++ait, ++count) {
            tob.ask_depth_k += ait->second.total_qty;
        }
    }
    return tob;
}

// ============================================================================
// OptimizedOrderBook (pre-allocated contiguous levels + fixed slot pool + flat hash)
// ============================================================================

OptimizedOrderBook::OptimizedOrderBook(size_t max_levels, size_t max_orders)
    : max_levels_(max_levels), max_orders_(max_orders) {
    bids_.reserve(max_levels_);
    asks_.reserve(max_levels_);
    pool_.resize(max_orders_);
    free_list_.reserve(max_orders_);
    for (size_t i = max_orders_; i > 0; --i) {
        free_list_.push_back(static_cast<uint32_t>(i - 1));
    }
    size_t table_size = next_power_of_two(std::max<size_t>(16, max_orders_ * 2 + 1));
    hash_table_.assign(table_size, HashEntry{kEmptyKey, kNullIdx});
    hash_mask_ = table_size - 1;
}

size_t OptimizedOrderBook::find_level(
    const std::vector<LevelEntry>& levels, int64_t price_ticks, bool descending, bool& found
) const {
    size_t lo = 0;
    size_t hi = levels.size();
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1U);
        int64_t mid_px = levels[mid].price_ticks;
        if (mid_px == price_ticks) {
            found = true;
            return mid;
        }
        bool go_right = descending ? (mid_px > price_ticks) : (mid_px < price_ticks);
        if (go_right) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    found = false;
    return lo;
}

uint32_t OptimizedOrderBook::alloc_slot() {
    if (free_list_.empty()) return kNullIdx;
    uint32_t idx = free_list_.back();
    free_list_.pop_back();
    pool_[idx] = OrderSlot{};
    pool_[idx].active = true;
    ++active_orders_;
    return idx;
}

void OptimizedOrderBook::free_slot(uint32_t slot) {
    if (slot == kNullIdx || !pool_[slot].active) return;
    pool_[slot].active = false;
    pool_[slot].order_id = 0;
    pool_[slot].prev = kNullIdx;
    pool_[slot].next = kNullIdx;
    free_list_.push_back(slot);
    --active_orders_;
}

bool OptimizedOrderBook::hash_insert(uint64_t order_id, uint32_t slot) {
    size_t idx = static_cast<size_t>(splitmix64(order_id)) & hash_mask_;
    for (size_t probe = 0; probe < hash_table_.size(); ++probe) {
        size_t pos = (idx + probe) & hash_mask_;
        if (hash_table_[pos].key == kEmptyKey) {
            hash_table_[pos].key = order_id;
            hash_table_[pos].slot = slot;
            return true;
        }
        if (hash_table_[pos].key == order_id) {
            return false;
        }
    }
    return false;
}

uint32_t OptimizedOrderBook::hash_find(uint64_t order_id) const {
    size_t idx = static_cast<size_t>(splitmix64(order_id)) & hash_mask_;
    for (size_t probe = 0; probe < hash_table_.size(); ++probe) {
        size_t pos = (idx + probe) & hash_mask_;
        uint64_t key = hash_table_[pos].key;
        if (key == kEmptyKey) return kNullIdx;
        if (key == order_id) return hash_table_[pos].slot;
    }
    return kNullIdx;
}

// Backward-shift deletion (Knuth Algorithm R): zero tombstones, constant O(1) probe chains.
void OptimizedOrderBook::hash_erase(uint64_t order_id) {
    size_t idx = static_cast<size_t>(splitmix64(order_id)) & hash_mask_;
    size_t pos = hash_table_.size();
    for (size_t probe = 0; probe < hash_table_.size(); ++probe) {
        size_t cur = (idx + probe) & hash_mask_;
        if (hash_table_[cur].key == kEmptyKey) return;
        if (hash_table_[cur].key == order_id) {
            pos = cur;
            break;
        }
    }
    if (pos == hash_table_.size()) return;

    size_t i = pos;
    while (true) {
        hash_table_[i].key = kEmptyKey;
        hash_table_[i].slot = kNullIdx;
        size_t j = i;
        while (true) {
            j = (j + 1) & hash_mask_;
            if (hash_table_[j].key == kEmptyKey) {
                return;
            }
            size_t r = static_cast<size_t>(splitmix64(hash_table_[j].key)) & hash_mask_;
            bool cyclic_between =
                (i <= j) ? (i < r && r <= j) : (i < r || r <= j);
            if (!cyclic_between) {
                hash_table_[i] = hash_table_[j];
                i = j;
                break;
            }
        }
    }
}

void OptimizedOrderBook::append_to_level(LevelEntry& level, uint32_t slot) {
    pool_[slot].prev = level.tail;
    pool_[slot].next = kNullIdx;
    if (level.tail != kNullIdx) {
        pool_[level.tail].next = slot;
    } else {
        level.head = slot;
    }
    level.tail = slot;
    level.total_qty += pool_[slot].qty;
    ++level.order_count;
}

void OptimizedOrderBook::unlink_from_level(std::vector<LevelEntry>& levels, size_t level_idx, uint32_t slot) {
    auto& lvl = levels[level_idx];
    uint32_t p = pool_[slot].prev;
    uint32_t n = pool_[slot].next;
    if (p != kNullIdx) {
        pool_[p].next = n;
    } else {
        lvl.head = n;
    }
    if (n != kNullIdx) {
        pool_[n].prev = p;
    } else {
        lvl.tail = p;
    }
    pool_[slot].prev = kNullIdx;
    pool_[slot].next = kNullIdx;
    lvl.total_qty -= pool_[slot].qty;
    if (lvl.order_count > 0) --lvl.order_count;
    if (lvl.total_qty <= 0 && lvl.order_count == 0) {
        levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(level_idx));
    }
}

void OptimizedOrderBook::clear(Side side) {
    auto clear_side = [&](std::vector<LevelEntry>& levels) {
        for (auto& lvl : levels) {
            uint32_t cur = lvl.head;
            while (cur != kNullIdx) {
                uint32_t nxt = pool_[cur].next;
                hash_erase(pool_[cur].order_id);
                free_slot(cur);
                cur = nxt;
            }
        }
        levels.clear();
    };

    if (side == Side::UNKNOWN) {
        clear_side(bids_);
        clear_side(asks_);
        return;
    }
    if (side == Side::BID) {
        clear_side(bids_);
    } else if (side == Side::ASK) {
        clear_side(asks_);
    }
}

BookErrorCode OptimizedOrderBook::set_l2_level(Side side, int64_t price_ticks, int64_t qty) {
    if (side == Side::UNKNOWN) return BookErrorCode::INVALID_SIDE;
    if (price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (qty < 0) return BookErrorCode::INVALID_QTY;

    bool desc = (side == Side::BID);
    auto& levels = desc ? bids_ : asks_;
    bool found = false;
    size_t pos = find_level(levels, price_ticks, desc, found);

    if (qty == 0) {
        if (found) {
            uint32_t cur = levels[pos].head;
            while (cur != kNullIdx) {
                uint32_t nxt = pool_[cur].next;
                hash_erase(pool_[cur].order_id);
                free_slot(cur);
                cur = nxt;
            }
            levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(pos));
        }
        return BookErrorCode::OK;
    }

    if (!found) {
        if (levels.size() >= max_levels_) {
            return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
        }
        LevelEntry entry{};
        entry.price_ticks = price_ticks;
        entry.total_qty = qty;
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(pos), entry);
        return BookErrorCode::OK;
    }

    uint32_t cur = levels[pos].head;
    while (cur != kNullIdx) {
        uint32_t nxt = pool_[cur].next;
        hash_erase(pool_[cur].order_id);
        free_slot(cur);
        cur = nxt;
    }
    levels[pos].head = kNullIdx;
    levels[pos].tail = kNullIdx;
    levels[pos].order_count = 0;
    levels[pos].total_qty = qty;
    return BookErrorCode::OK;
}

BookErrorCode OptimizedOrderBook::add_l3_order(uint64_t order_id, Side side, int64_t price_ticks, int64_t qty) {
    if (order_id == 0 || order_id == kTombstoneKey) return BookErrorCode::ORDER_NOT_FOUND;
    if (side == Side::UNKNOWN) return BookErrorCode::INVALID_SIDE;
    if (price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (qty <= 0) return BookErrorCode::INVALID_QTY;

    if (hash_find(order_id) != kNullIdx) {
        return BookErrorCode::DUPLICATE_ORDER_ID;
    }
    if (active_orders_ >= max_orders_) {
        return BookErrorCode::CAPACITY_ORDERS_EXCEEDED;
    }

    bool desc = (side == Side::BID);
    auto& levels = desc ? bids_ : asks_;
    bool found = false;
    size_t pos = find_level(levels, price_ticks, desc, found);
    if (!found) {
        if (levels.size() >= max_levels_) {
            return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
        }
        LevelEntry entry{};
        entry.price_ticks = price_ticks;
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(pos), entry);
    }

    uint32_t slot = alloc_slot();
    if (slot == kNullIdx) {
        return BookErrorCode::CAPACITY_ORDERS_EXCEEDED;
    }
    pool_[slot].order_id = order_id;
    pool_[slot].side = side;
    pool_[slot].price_ticks = price_ticks;
    pool_[slot].qty = qty;

    hash_insert(order_id, slot);
    append_to_level(levels[pos], slot);
    return BookErrorCode::OK;
}

BookErrorCode OptimizedOrderBook::modify_l3_order(uint64_t order_id, int64_t new_price_ticks, int64_t new_qty) {
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (new_price_ticks <= 0) return BookErrorCode::INVALID_PRICE;
    if (new_qty < 0) return BookErrorCode::INVALID_QTY;

    uint32_t slot = hash_find(order_id);
    if (slot == kNullIdx) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = pool_[slot].side;
    int64_t old_price = pool_[slot].price_ticks;
    int64_t old_qty = pool_[slot].qty;
    bool desc = (side == Side::BID);
    auto& levels = desc ? bids_ : asks_;

    bool old_found = false;
    size_t old_pos = find_level(levels, old_price, desc, old_found);
    if (!old_found) return BookErrorCode::ORDER_NOT_FOUND;

    if (new_qty == 0) {
        unlink_from_level(levels, old_pos, slot);
        hash_erase(order_id);
        free_slot(slot);
        return BookErrorCode::OK;
    }

    if (new_price_ticks == old_price && new_qty <= old_qty) {
        levels[old_pos].total_qty += (new_qty - old_qty);
        pool_[slot].qty = new_qty;
        return BookErrorCode::OK;
    }

    if (new_price_ticks != old_price) {
        bool new_found = false;
        (void)find_level(levels, new_price_ticks, desc, new_found);
        bool old_will_vanish = (levels[old_pos].order_count == 1 && levels[old_pos].total_qty == old_qty);
        if (!new_found && !old_will_vanish && levels.size() >= max_levels_) {
            return BookErrorCode::CAPACITY_LEVELS_EXCEEDED;
        }
    }

    unlink_from_level(levels, old_pos, slot);
    pool_[slot].price_ticks = new_price_ticks;
    pool_[slot].qty = new_qty;

    bool new_found = false;
    size_t new_pos = find_level(levels, new_price_ticks, desc, new_found);
    if (!new_found) {
        LevelEntry entry{};
        entry.price_ticks = new_price_ticks;
        levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(new_pos), entry);
    }
    append_to_level(levels[new_pos], slot);
    return BookErrorCode::OK;
}

BookErrorCode OptimizedOrderBook::cancel_l3_order(uint64_t order_id, int64_t cancel_qty) {
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (cancel_qty < 0) return BookErrorCode::INVALID_QTY;

    uint32_t slot = hash_find(order_id);
    if (slot == kNullIdx) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = pool_[slot].side;
    int64_t price = pool_[slot].price_ticks;
    bool desc = (side == Side::BID);
    auto& levels = desc ? bids_ : asks_;
    bool found = false;
    size_t pos = find_level(levels, price, desc, found);
    if (!found) return BookErrorCode::ORDER_NOT_FOUND;

    if (cancel_qty == 0 || cancel_qty >= pool_[slot].qty) {
        unlink_from_level(levels, pos, slot);
        hash_erase(order_id);
        free_slot(slot);
    } else {
        pool_[slot].qty -= cancel_qty;
        levels[pos].total_qty -= cancel_qty;
    }
    return BookErrorCode::OK;
}

BookErrorCode OptimizedOrderBook::execute_l3_order(uint64_t order_id, int64_t exec_qty, QueueSnapshot& pre_exec_queue) {
    pre_exec_queue = QueueSnapshot{};
    if (order_id == 0) return BookErrorCode::ORDER_NOT_FOUND;
    if (exec_qty <= 0) return BookErrorCode::INVALID_QTY;

    uint32_t slot = hash_find(order_id);
    if (slot == kNullIdx) {
        return BookErrorCode::ORDER_NOT_FOUND;
    }

    Side side = pool_[slot].side;
    int64_t price = pool_[slot].price_ticks;
    bool desc = (side == Side::BID);
    auto& levels = desc ? bids_ : asks_;
    bool found = false;
    size_t pos = find_level(levels, price, desc, found);
    if (!found) return BookErrorCode::ORDER_NOT_FOUND;

    int64_t ahead_qty = 0;
    size_t ahead_orders = 0;
    uint32_t cur = levels[pos].head;
    while (cur != kNullIdx && cur != slot) {
        ahead_qty += pool_[cur].qty;
        ++ahead_orders;
        cur = pool_[cur].next;
    }

    pre_exec_queue.found = true;
    pre_exec_queue.side = side;
    pre_exec_queue.price_ticks = price;
    pre_exec_queue.queue_ahead_qty = ahead_qty;
    pre_exec_queue.queue_ahead_orders = ahead_orders;
    pre_exec_queue.level_total_qty = levels[pos].total_qty;
    pre_exec_queue.queue_position_ratio =
        (levels[pos].total_qty > 0)
            ? static_cast<double>(ahead_qty) / static_cast<double>(levels[pos].total_qty)
            : 0.0;

    if (exec_qty >= pool_[slot].qty) {
        unlink_from_level(levels, pos, slot);
        hash_erase(order_id);
        free_slot(slot);
    } else {
        pool_[slot].qty -= exec_qty;
        levels[pos].total_qty -= exec_qty;
    }
    return BookErrorCode::OK;
}

TopOfBook OptimizedOrderBook::top_of_book(size_t depth_k) const {
    TopOfBook tob{};
    if (!bids_.empty()) {
        tob.has_bid = true;
        tob.best_bid_ticks = bids_[0].price_ticks;
        tob.bid_qty_l1 = bids_[0].total_qty;
        tob.bid_l1_orders = bids_[0].order_count;
        size_t lim = std::min(depth_k, bids_.size());
        for (size_t i = 0; i < lim; ++i) {
            tob.bid_depth_k += bids_[i].total_qty;
        }
    }
    if (!asks_.empty()) {
        tob.has_ask = true;
        tob.best_ask_ticks = asks_[0].price_ticks;
        tob.ask_qty_l1 = asks_[0].total_qty;
        tob.ask_l1_orders = asks_[0].order_count;
        size_t lim = std::min(depth_k, asks_.size());
        for (size_t i = 0; i < lim; ++i) {
            tob.ask_depth_k += asks_[i].total_qty;
        }
    }
    return tob;
}

std::unique_ptr<IOrderBook> make_order_book(const EngineConfig& config) {
    if (config.variant == "baseline") {
        return std::make_unique<BaselineOrderBook>(config.max_levels, config.max_orders);
    }
    if (config.variant == "optimized") {
        return std::make_unique<OptimizedOrderBook>(config.max_levels, config.max_orders);
    }
    throw std::invalid_argument("Unsupported engine variant: " + config.variant);
}

}  // namespace mmae
