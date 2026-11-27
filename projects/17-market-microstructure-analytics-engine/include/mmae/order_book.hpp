#pragma once

#include "mmae/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mmae {

enum class BookErrorCode : uint8_t {
    OK = 0,
    CAPACITY_LEVELS_EXCEEDED = 1,
    CAPACITY_ORDERS_EXCEEDED = 2,
    DUPLICATE_ORDER_ID = 3,
    ORDER_NOT_FOUND = 4,
    INVALID_PRICE = 5,
    INVALID_QTY = 6,
    INVALID_SIDE = 7
};

struct QueueSnapshot {
    bool found = false;
    Side side = Side::UNKNOWN;
    int64_t price_ticks = 0;
    int64_t queue_ahead_qty = 0;
    size_t queue_ahead_orders = 0;
    int64_t level_total_qty = 0;
    double queue_position_ratio = 0.0;
};

struct TopOfBook {
    bool has_bid = false;
    bool has_ask = false;
    int64_t best_bid_ticks = 0;
    int64_t best_ask_ticks = 0;
    int64_t bid_qty_l1 = 0;
    int64_t ask_qty_l1 = 0;
    size_t bid_l1_orders = 0;
    size_t ask_l1_orders = 0;
    int64_t bid_depth_k = 0;
    int64_t ask_depth_k = 0;
};

class IOrderBook {
public:
    virtual ~IOrderBook() = default;

    virtual void clear(Side side = Side::UNKNOWN) = 0;
    virtual BookErrorCode set_l2_level(Side side, int64_t price_ticks, int64_t qty) = 0;
    virtual BookErrorCode add_l3_order(uint64_t order_id, Side side, int64_t price_ticks, int64_t qty) = 0;
    virtual BookErrorCode modify_l3_order(uint64_t order_id, int64_t new_price_ticks, int64_t new_qty) = 0;
    virtual BookErrorCode cancel_l3_order(uint64_t order_id, int64_t cancel_qty) = 0;
    virtual BookErrorCode execute_l3_order(uint64_t order_id, int64_t exec_qty, QueueSnapshot& pre_exec_queue) = 0;

    virtual TopOfBook top_of_book(size_t depth_k) const = 0;
    virtual size_t order_count() const = 0;
    virtual size_t bid_level_count() const = 0;
    virtual size_t ask_level_count() const = 0;
    virtual const char* variant_name() const = 0;
};

// Reference baseline implementation using std::map + std::list + std::unordered_map.
class BaselineOrderBook final : public IOrderBook {
public:
    BaselineOrderBook(size_t max_levels, size_t max_orders);

    void clear(Side side = Side::UNKNOWN) override;
    BookErrorCode set_l2_level(Side side, int64_t price_ticks, int64_t qty) override;
    BookErrorCode add_l3_order(uint64_t order_id, Side side, int64_t price_ticks, int64_t qty) override;
    BookErrorCode modify_l3_order(uint64_t order_id, int64_t new_price_ticks, int64_t new_qty) override;
    BookErrorCode cancel_l3_order(uint64_t order_id, int64_t cancel_qty) override;
    BookErrorCode execute_l3_order(uint64_t order_id, int64_t exec_qty, QueueSnapshot& pre_exec_queue) override;

    TopOfBook top_of_book(size_t depth_k) const override;
    size_t order_count() const override { return orders_.size(); }
    size_t bid_level_count() const override { return bids_.size(); }
    size_t ask_level_count() const override { return asks_.size(); }
    const char* variant_name() const override { return "baseline"; }

private:
    struct OrderNode {
        uint64_t order_id = 0;
        int64_t qty = 0;
    };

    struct Level {
        int64_t total_qty = 0;
        std::list<OrderNode> queue;
    };

    struct OrderRef {
        Side side = Side::UNKNOWN;
        int64_t price_ticks = 0;
        std::list<OrderNode>::iterator it;
    };

    size_t max_levels_;
    size_t max_orders_;
    std::map<int64_t, Level, std::greater<int64_t>> bids_;
    std::map<int64_t, Level, std::less<int64_t>> asks_;
    std::unordered_map<uint64_t, OrderRef> orders_;
};

// Optimized bounded implementation using pre-allocated contiguous level arrays,
// an intrusive fixed-capacity order slot pool, and a flat open-addressing order index.
// Performs zero dynamic allocations on the hot path after construction.
class OptimizedOrderBook final : public IOrderBook {
public:
    OptimizedOrderBook(size_t max_levels, size_t max_orders);

    void clear(Side side = Side::UNKNOWN) override;
    BookErrorCode set_l2_level(Side side, int64_t price_ticks, int64_t qty) override;
    BookErrorCode add_l3_order(uint64_t order_id, Side side, int64_t price_ticks, int64_t qty) override;
    BookErrorCode modify_l3_order(uint64_t order_id, int64_t new_price_ticks, int64_t new_qty) override;
    BookErrorCode cancel_l3_order(uint64_t order_id, int64_t cancel_qty) override;
    BookErrorCode execute_l3_order(uint64_t order_id, int64_t exec_qty, QueueSnapshot& pre_exec_queue) override;

    TopOfBook top_of_book(size_t depth_k) const override;
    size_t order_count() const override { return active_orders_; }
    size_t bid_level_count() const override { return bids_.size(); }
    size_t ask_level_count() const override { return asks_.size(); }
    const char* variant_name() const override { return "optimized"; }

private:
    static constexpr uint32_t kNullIdx = std::numeric_limits<uint32_t>::max();
    static constexpr uint64_t kEmptyKey = 0;
    static constexpr uint64_t kTombstoneKey = std::numeric_limits<uint64_t>::max();

    struct OrderSlot {
        uint64_t order_id = 0;
        Side side = Side::UNKNOWN;
        int64_t price_ticks = 0;
        int64_t qty = 0;
        uint32_t prev = kNullIdx;
        uint32_t next = kNullIdx;
        bool active = false;
    };

    struct LevelEntry {
        int64_t price_ticks = 0;
        int64_t total_qty = 0;
        uint32_t head = kNullIdx;
        uint32_t tail = kNullIdx;
        uint32_t order_count = 0;
    };

    struct HashEntry {
        uint64_t key = kEmptyKey;
        uint32_t slot = kNullIdx;
    };

    size_t find_level(const std::vector<LevelEntry>& levels, int64_t price_ticks, bool descending, bool& found) const;
    uint32_t alloc_slot();
    void free_slot(uint32_t slot);
    bool hash_insert(uint64_t order_id, uint32_t slot);
    uint32_t hash_find(uint64_t order_id) const;
    void hash_erase(uint64_t order_id);
    void unlink_from_level(std::vector<LevelEntry>& levels, size_t level_idx, uint32_t slot);
    void append_to_level(LevelEntry& level, uint32_t slot);

    size_t max_levels_;
    size_t max_orders_;
    size_t active_orders_ = 0;
    std::vector<LevelEntry> bids_;  // sorted descending by price_ticks
    std::vector<LevelEntry> asks_;  // sorted ascending by price_ticks
    std::vector<OrderSlot> pool_;
    std::vector<uint32_t> free_list_;
    std::vector<HashEntry> hash_table_;
    size_t hash_mask_ = 0;
};

std::unique_ptr<IOrderBook> make_order_book(const EngineConfig& config);

}  // namespace mmae
