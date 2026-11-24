#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "mdf/protocol.hpp"

namespace mdf {

struct Order {
  uint32_t instrument_id = 0;
  uint64_t order_id = 0;
  Side side = Side::kBid;
  int64_t price_ticks = 0;
  uint32_t qty = 0;

  bool operator==(const Order&) const = default;
};

struct PriceLevel {
  int64_t price_ticks = 0;
  uint64_t total_qty = 0;
  uint32_t order_count = 0;

  bool operator==(const PriceLevel&) const = default;
};

struct InstrumentStats {
  int64_t last_trade_price_ticks = 0;
  uint32_t last_trade_qty = 0;
  uint64_t trade_count = 0;
  uint64_t total_volume = 0;

  bool operator==(const InstrumentStats&) const = default;
};

class InstrumentBook {
 public:
  void add_order(const Order& order);
  void modify_order(uint64_t order_id, int64_t new_price_ticks, uint32_t new_qty);
  void cancel_order(uint64_t order_id, uint32_t cancel_qty);
  void apply_trade(uint64_t resting_order_id, int64_t exec_price_ticks, uint32_t exec_qty);
  void clear();

  std::optional<PriceLevel> best_bid() const;
  std::optional<PriceLevel> best_ask() const;
  std::size_t order_count() const noexcept { return orders_.size(); }
  const std::unordered_map<uint64_t, Order>& orders() const noexcept { return orders_; }
  const std::map<int64_t, PriceLevel, std::greater<int64_t>>& bids() const noexcept {
    return bids_;
  }
  const std::map<int64_t, PriceLevel, std::less<int64_t>>& asks() const noexcept { return asks_; }
  const InstrumentStats& stats() const noexcept { return stats_; }
  void set_stats(const InstrumentStats& st) noexcept { stats_ = st; }

  bool operator==(const InstrumentBook& other) const;

 private:
  void add_to_level(Side side, int64_t price, uint32_t qty);
  void remove_from_level(Side side, int64_t price, uint32_t qty, bool decrement_order_count);

  std::unordered_map<uint64_t, Order> orders_;
  std::map<int64_t, PriceLevel, std::greater<int64_t>> bids_;
  std::map<int64_t, PriceLevel, std::less<int64_t>> asks_;
  InstrumentStats stats_{};
};

// Deterministic per-channel market state holding all instruments for one feed channel.
class ChannelMarketState {
 public:
  explicit ChannelMarketState(uint16_t channel_id = 0) : channel_id_(channel_id) {}

  uint16_t channel_id() const noexcept { return channel_id_; }
  uint64_t last_applied_seq() const noexcept { return last_applied_seq_; }
  uint64_t total_events_applied() const noexcept { return total_events_applied_; }

  void apply_event(const MarketEvent& ev);
  void apply_packet(uint64_t seq_num, std::span<const MarketEvent> events);

  // Export/import consistent point-in-time snapshot at `last_applied_seq()`.
  SnapshotData export_snapshot(uint64_t checkpoint_id) const;
  void load_snapshot(const SnapshotData& snap);
  void clear();

  const InstrumentBook* find_instrument(uint32_t instrument_id) const;
  const std::map<uint32_t, InstrumentBook>& instruments() const noexcept { return instruments_; }

  std::size_t total_active_orders() const noexcept;

  // Canonical 64-bit FNV-1a digest over channel_id, last_applied_seq, sorted orders,
  // aggregated L2 levels, and trade statistics. Independent of hash map iteration order.
  uint64_t state_hash() const;

  bool operator==(const ChannelMarketState& other) const;

  // Human-readable diff if two states diverge (empty string if identical).
  std::string diff(const ChannelMarketState& other) const;

 private:
  uint16_t channel_id_ = 0;
  uint64_t last_applied_seq_ = 0;
  uint64_t total_events_applied_ = 0;
  std::map<uint32_t, InstrumentBook> instruments_;
};

}  // namespace mdf
