#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "lpl/allocator_bench.hpp"
#include "lpl/flat_hash_map.hpp"

namespace lpl {

enum class OrderAction : uint8_t {
  kAddLimit = 1,
  kCancel = 2,
  kModifyQty = 3,
  kAddMarket = 4,
};

enum class Side : uint8_t {
  kBuy = 0,
  kSell = 1,
};

struct OrderCommand {
  OrderAction action = OrderAction::kAddLimit;
  Side side = Side::kBuy;
  uint64_t order_id = 0;
  int64_t price = 0;
  uint32_t qty = 0;
};

struct Trade {
  uint64_t aggressor_id = 0;
  uint64_t resting_id = 0;
  int64_t price = 0;
  uint32_t qty = 0;

  bool operator==(const Trade&) const noexcept = default;
};

struct EngineSummary {
  uint64_t orders_accepted = 0;
  uint64_t orders_rejected = 0;
  uint64_t cancels_succeeded = 0;
  uint64_t cancels_missed = 0;
  uint64_t trades_executed = 0;
  uint64_t total_traded_qty = 0;
  int64_t best_bid = 0;
  int64_t best_ask = 0;
  size_t resting_orders = 0;
  uint64_t trade_checksum = 0;

  bool operator==(const EngineSummary&) const noexcept = default;
};

// Binary wire codec for deterministic order streams (used for replay & fuzz tests).
// Header (16 bytes LE): magic "LPLO" (4B), version=1 (2B), reserved (2B), record_count (8B).
// Record (24 bytes LE): action (1B), side (1B), reserved (2B), qty (4B), order_id (8B), price (8B).
struct DecodeResult {
  bool ok = false;
  std::string error;
  std::vector<OrderCommand> commands;
};

std::vector<uint8_t> encode_order_stream(const std::vector<OrderCommand>& commands);
DecodeResult decode_order_stream(std::span<const uint8_t> bytes);

// Realistic deterministic synthetic order flow generator (mix of resting limits,
// crossing limits, market orders, size reductions, and cancellations).
std::vector<OrderCommand> generate_matching_workload(size_t num_commands, uint64_t seed = 4242);

// Engine A (Baseline): std::map price levels + std::list FIFO queues + std::unordered_map index.
class BaselineMatchingEngine {
 public:
  void reset();
  void process(const OrderCommand& cmd, std::vector<Trade>* out_trades = nullptr);
  EngineSummary summary() const noexcept;

 private:
  struct RestingOrder {
    uint64_t order_id;
    int64_t price;
    uint32_t qty;
    Side side;
  };

  using OrderList = std::list<RestingOrder>;
  using BidMap = std::map<int64_t, OrderList, std::greater<int64_t>>;
  using AskMap = std::map<int64_t, OrderList, std::less<int64_t>>;

  struct OrderLocator {
    Side side;
    int64_t price;
    OrderList::iterator it;
  };

  void handle_add(const OrderCommand& cmd, bool is_market, std::vector<Trade>* out_trades);
  void handle_cancel(const OrderCommand& cmd);
  void handle_modify(const OrderCommand& cmd);
  void record_trade(uint64_t aggressor, uint64_t resting, int64_t price, uint32_t qty,
                    std::vector<Trade>* out_trades);

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<uint64_t, OrderLocator> index_;

  uint64_t orders_accepted_ = 0;
  uint64_t orders_rejected_ = 0;
  uint64_t cancels_succeeded_ = 0;
  uint64_t cancels_missed_ = 0;
  uint64_t trades_executed_ = 0;
  uint64_t total_traded_qty_ = 0;
  uint64_t trade_checksum_ = 0;
};

// Engine B (Optimized): ObjectPool<IntrusiveOrder> + intrusive FIFO queues +
// contiguous sorted price-level vectors (BBO at back()) + open-addressing FlatHashMap index.
class PooledFlatMatchingEngine {
 public:
  explicit PooledFlatMatchingEngine(size_t max_resting_orders = 262'144);
  ~PooledFlatMatchingEngine();

  PooledFlatMatchingEngine(const PooledFlatMatchingEngine&) = delete;
  PooledFlatMatchingEngine& operator=(const PooledFlatMatchingEngine&) = delete;

  void reset();
  void process(const OrderCommand& cmd, std::vector<Trade>* out_trades = nullptr);
  EngineSummary summary() const noexcept;

 private:
  struct IntrusiveOrder {
    uint64_t order_id = 0;
    int64_t price = 0;
    uint32_t qty = 0;
    Side side = Side::kBuy;
    IntrusiveOrder* prev = nullptr;
    IntrusiveOrder* next = nullptr;
  };

  struct LevelQueue {
    int64_t price = 0;
    IntrusiveOrder* head = nullptr;
    IntrusiveOrder* tail = nullptr;
  };

  void handle_add(const OrderCommand& cmd, bool is_market, std::vector<Trade>* out_trades);
  void handle_cancel(const OrderCommand& cmd);
  void handle_modify(const OrderCommand& cmd);
  void record_trade(uint64_t aggressor, uint64_t resting, int64_t price, uint32_t qty,
                    std::vector<Trade>* out_trades);

  // Bids sorted ascending (best bid at back()); asks sorted descending (best ask at back()).
  LevelQueue* find_or_insert_level(std::vector<LevelQueue>& levels, int64_t price, bool is_bid);
  void unlink_order(std::vector<LevelQueue>& levels, IntrusiveOrder* node, bool is_bid);

  ObjectPool<IntrusiveOrder> pool_;
  std::vector<LevelQueue> bids_;
  std::vector<LevelQueue> asks_;
  FlatHashMap<uint64_t, IntrusiveOrder*> index_;

  uint64_t orders_accepted_ = 0;
  uint64_t orders_rejected_ = 0;
  uint64_t cancels_succeeded_ = 0;
  uint64_t cancels_missed_ = 0;
  uint64_t trades_executed_ = 0;
  uint64_t total_traded_qty_ = 0;
  uint64_t trade_checksum_ = 0;
};

}  // namespace lpl
