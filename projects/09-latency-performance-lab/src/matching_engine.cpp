#include "lpl/matching_engine.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>

namespace lpl {

namespace {

constexpr uint8_t kStreamMagic[4] = {'L', 'P', 'L', 'O'};
constexpr uint16_t kStreamVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr size_t kRecordSize = 24;
constexpr size_t kMaxDecodedRecords = 50'000'000;

inline uint64_t mix_trade_checksum(uint64_t cur, uint64_t agg, uint64_t rest, int64_t price,
                                   uint32_t qty) noexcept {
  uint64_t h = cur ^ 0x9e3779b97f4a7c15ULL;
  h = (h * 0xbf58476d1ce4e5b9ULL) ^ agg;
  h = (h * 0x94d049bb133111ebULL) ^ rest;
  h = (h * 0xbf58476d1ce4e5b9ULL) ^ static_cast<uint64_t>(price);
  h = (h * 0x94d049bb133111ebULL) ^ static_cast<uint64_t>(qty);
  return h ^ (h >> 31);
}

void write_u16_le(uint8_t* p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xFFU);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void write_u32_le(uint8_t* p, uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFU);
}

void write_u64_le(uint8_t* p, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFU);
}

uint16_t read_u16_le(const uint8_t* p) noexcept {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

uint64_t read_u64_le(const uint8_t* p) noexcept {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

std::vector<uint8_t> encode_order_stream(const std::vector<OrderCommand>& commands) {
  std::vector<uint8_t> buf(kHeaderSize + commands.size() * kRecordSize, 0);
  std::memcpy(buf.data(), kStreamMagic, 4);
  write_u16_le(buf.data() + 4, kStreamVersion);
  write_u16_le(buf.data() + 6, 0);
  write_u64_le(buf.data() + 8, static_cast<uint64_t>(commands.size()));

  for (size_t i = 0; i < commands.size(); ++i) {
    uint8_t* rec = buf.data() + kHeaderSize + i * kRecordSize;
    const auto& c = commands[i];
    rec[0] = static_cast<uint8_t>(c.action);
    rec[1] = static_cast<uint8_t>(c.side);
    rec[2] = 0;
    rec[3] = 0;
    write_u32_le(rec + 4, c.qty);
    write_u64_le(rec + 8, c.order_id);
    write_u64_le(rec + 16, static_cast<uint64_t>(c.price));
  }
  return buf;
}

DecodeResult decode_order_stream(std::span<const uint8_t> bytes) {
  DecodeResult res;
  if (bytes.size() < kHeaderSize) {
    res.error = "truncated header";
    return res;
  }
  if (std::memcmp(bytes.data(), kStreamMagic, 4) != 0) {
    res.error = "invalid magic bytes";
    return res;
  }
  const uint16_t version = read_u16_le(bytes.data() + 4);
  if (version != kStreamVersion) {
    res.error = "unsupported stream version";
    return res;
  }
  const uint64_t count = read_u64_le(bytes.data() + 8);
  if (count > kMaxDecodedRecords) {
    res.error = "record count exceeds maximum";
    return res;
  }
  const uint64_t expected_bytes = kHeaderSize + count * kRecordSize;
  if (bytes.size() != expected_bytes) {
    res.error = "payload size mismatch";
    return res;
  }

  res.commands.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    const uint8_t* rec = bytes.data() + kHeaderSize + static_cast<size_t>(i) * kRecordSize;
    const uint8_t action_raw = rec[0];
    const uint8_t side_raw = rec[1];
    if (action_raw < 1 || action_raw > 4) {
      res.error = "invalid action enum";
      res.commands.clear();
      return res;
    }
    if (side_raw > 1) {
      res.error = "invalid side enum";
      res.commands.clear();
      return res;
    }
    OrderCommand cmd;
    cmd.action = static_cast<OrderAction>(action_raw);
    cmd.side = static_cast<Side>(side_raw);
    cmd.qty = read_u32_le(rec + 4);
    cmd.order_id = read_u64_le(rec + 8);
    cmd.price = static_cast<int64_t>(read_u64_le(rec + 16));
    res.commands.push_back(cmd);
  }

  res.ok = true;
  return res;
}

std::vector<OrderCommand> generate_matching_workload(size_t num_commands, uint64_t seed) {
  std::vector<OrderCommand> cmds;
  cmds.reserve(num_commands);

  std::mt19937_64 rng(seed);
  int64_t mid = 100'000;
  uint64_t next_id = 1;
  std::vector<uint64_t> active_ids;
  active_ids.reserve(4096);

  std::geometric_distribution<int64_t> depth_dist(0.20);
  std::uniform_int_distribution<uint32_t> qty_dist(10, 400);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (size_t i = 0; i < num_commands; ++i) {
    // Gentle random walk on mid-price every 64 events.
    if ((i & 63U) == 0 && i > 0) {
      const int step = static_cast<int>(rng() % 3ULL) - 1;
      mid = std::max<int64_t>(1000, mid + step);
    }

    const int roll = op_dist(rng);
    if (roll < 55 || active_ids.size() < 32) {
      // Resting or mildly crossing limit order
      const Side side = (rng() & 1ULL) ? Side::kBuy : Side::kSell;
      const int64_t offset = depth_dist(rng) % 40;
      const bool aggressive = (rng() % 100) < 22;
      int64_t price = 0;
      if (side == Side::kBuy) {
        price = aggressive ? (mid + 1 + (offset % 3)) : (mid - 1 - offset);
      } else {
        price = aggressive ? (mid - 1 - (offset % 3)) : (mid + 1 + offset);
      }
      price = std::max<int64_t>(1, price);
      const uint64_t oid = next_id++;
      active_ids.push_back(oid);
      cmds.push_back(OrderCommand{OrderAction::kAddLimit, side, oid, price, qty_dist(rng)});
    } else if (roll < 82) {
      // Cancel an existing active order (or recently filled order -> tests cancel miss path)
      const size_t idx = rng() % active_ids.size();
      const uint64_t oid = active_ids[idx];
      active_ids[idx] = active_ids.back();
      active_ids.pop_back();
      cmds.push_back(OrderCommand{OrderAction::kCancel, Side::kBuy, oid, 0, 0});
    } else if (roll < 94) {
      // Modify qty down on an existing order
      const size_t idx = rng() % active_ids.size();
      const uint64_t oid = active_ids[idx];
      cmds.push_back(OrderCommand{OrderAction::kModifyQty, Side::kBuy, oid, 0,
                                  static_cast<uint32_t>(5 + (rng() % 50))});
    } else {
      // Market order sweeping top of book
      const Side side = (rng() & 1ULL) ? Side::kBuy : Side::kSell;
      const uint64_t oid = next_id++;
      cmds.push_back(OrderCommand{OrderAction::kAddMarket, side, oid, 0,
                                  static_cast<uint32_t>(10 + (rng() % 250))});
    }
  }
  return cmds;
}

// ========================= BaselineMatchingEngine =========================

void BaselineMatchingEngine::reset() {
  bids_.clear();
  asks_.clear();
  index_.clear();
  orders_accepted_ = 0;
  orders_rejected_ = 0;
  cancels_succeeded_ = 0;
  cancels_missed_ = 0;
  trades_executed_ = 0;
  total_traded_qty_ = 0;
  trade_checksum_ = 0;
}

void BaselineMatchingEngine::record_trade(uint64_t aggressor, uint64_t resting, int64_t price,
                                          uint32_t qty, std::vector<Trade>* out_trades) {
  ++trades_executed_;
  total_traded_qty_ += qty;
  trade_checksum_ = mix_trade_checksum(trade_checksum_, aggressor, resting, price, qty);
  if (out_trades != nullptr) {
    out_trades->push_back(Trade{aggressor, resting, price, qty});
  }
}

void BaselineMatchingEngine::process(const OrderCommand& cmd, std::vector<Trade>* out_trades) {
  switch (cmd.action) {
    case OrderAction::kAddLimit:
      handle_add(cmd, false, out_trades);
      break;
    case OrderAction::kAddMarket:
      handle_add(cmd, true, out_trades);
      break;
    case OrderAction::kCancel:
      handle_cancel(cmd);
      break;
    case OrderAction::kModifyQty:
      handle_modify(cmd);
      break;
  }
}

void BaselineMatchingEngine::handle_add(const OrderCommand& cmd, bool is_market,
                                        std::vector<Trade>* out_trades) {
  if (cmd.order_id == 0 || cmd.order_id == std::numeric_limits<uint64_t>::max() || cmd.qty == 0 ||
      (!is_market && cmd.price <= 0) || index_.find(cmd.order_id) != index_.end()) {
    ++orders_rejected_;
    return;
  }
  ++orders_accepted_;
  uint32_t remaining = cmd.qty;

  if (cmd.side == Side::kBuy) {
    while (remaining > 0 && !asks_.empty()) {
      auto best_it = asks_.begin();
      if (!is_market && best_it->first > cmd.price) break;
      auto& queue = best_it->second;
      while (remaining > 0 && !queue.empty()) {
        auto& resting = queue.front();
        const uint32_t fill = std::min(remaining, resting.qty);
        record_trade(cmd.order_id, resting.order_id, resting.price, fill, out_trades);
        remaining -= fill;
        resting.qty -= fill;
        if (resting.qty == 0) {
          index_.erase(resting.order_id);
          queue.pop_front();
        }
      }
      if (queue.empty()) {
        asks_.erase(best_it);
      }
    }
    if (!is_market && remaining > 0) {
      auto& queue = bids_[cmd.price];
      queue.push_back(RestingOrder{cmd.order_id, cmd.price, remaining, Side::kBuy});
      index_.emplace(cmd.order_id, OrderLocator{Side::kBuy, cmd.price, std::prev(queue.end())});
    }
  } else {
    while (remaining > 0 && !bids_.empty()) {
      auto best_it = bids_.begin();
      if (!is_market && best_it->first < cmd.price) break;
      auto& queue = best_it->second;
      while (remaining > 0 && !queue.empty()) {
        auto& resting = queue.front();
        const uint32_t fill = std::min(remaining, resting.qty);
        record_trade(cmd.order_id, resting.order_id, resting.price, fill, out_trades);
        remaining -= fill;
        resting.qty -= fill;
        if (resting.qty == 0) {
          index_.erase(resting.order_id);
          queue.pop_front();
        }
      }
      if (queue.empty()) {
        bids_.erase(best_it);
      }
    }
    if (!is_market && remaining > 0) {
      auto& queue = asks_[cmd.price];
      queue.push_back(RestingOrder{cmd.order_id, cmd.price, remaining, Side::kSell});
      index_.emplace(cmd.order_id, OrderLocator{Side::kSell, cmd.price, std::prev(queue.end())});
    }
  }
}

void BaselineMatchingEngine::handle_cancel(const OrderCommand& cmd) {
  const auto it = index_.find(cmd.order_id);
  if (it == index_.end()) {
    ++cancels_missed_;
    return;
  }
  const OrderLocator loc = it->second;
  index_.erase(it);
  if (loc.side == Side::kBuy) {
    auto lvl_it = bids_.find(loc.price);
    if (lvl_it != bids_.end()) {
      lvl_it->second.erase(loc.it);
      if (lvl_it->second.empty()) bids_.erase(lvl_it);
    }
  } else {
    auto lvl_it = asks_.find(loc.price);
    if (lvl_it != asks_.end()) {
      lvl_it->second.erase(loc.it);
      if (lvl_it->second.empty()) asks_.erase(lvl_it);
    }
  }
  ++cancels_succeeded_;
}

void BaselineMatchingEngine::handle_modify(const OrderCommand& cmd) {
  const auto it = index_.find(cmd.order_id);
  if (it == index_.end()) {
    ++orders_rejected_;
    return;
  }
  if (cmd.qty == 0) {
    handle_cancel(cmd);
    return;
  }
  if (cmd.qty > it->second.it->qty) {
    ++orders_rejected_;
    return;
  }
  it->second.it->qty = cmd.qty;
  ++orders_accepted_;
}

EngineSummary BaselineMatchingEngine::summary() const noexcept {
  EngineSummary s;
  s.orders_accepted = orders_accepted_;
  s.orders_rejected = orders_rejected_;
  s.cancels_succeeded = cancels_succeeded_;
  s.cancels_missed = cancels_missed_;
  s.trades_executed = trades_executed_;
  s.total_traded_qty = total_traded_qty_;
  s.best_bid = bids_.empty() ? 0 : bids_.begin()->first;
  s.best_ask = asks_.empty() ? 0 : asks_.begin()->first;
  s.resting_orders = index_.size();
  s.trade_checksum = trade_checksum_;
  return s;
}

// ========================= PooledFlatMatchingEngine =========================

PooledFlatMatchingEngine::PooledFlatMatchingEngine(size_t max_resting_orders)
    : pool_(max_resting_orders), index_(std::max<size_t>(1024, max_resting_orders / 4)) {
  bids_.reserve(512);
  asks_.reserve(512);
}

PooledFlatMatchingEngine::~PooledFlatMatchingEngine() { reset(); }

void PooledFlatMatchingEngine::reset() {
  for (auto& lvl : bids_) {
    IntrusiveOrder* cur = lvl.head;
    while (cur != nullptr) {
      IntrusiveOrder* nxt = cur->next;
      pool_.destroy(cur);
      cur = nxt;
    }
  }
  for (auto& lvl : asks_) {
    IntrusiveOrder* cur = lvl.head;
    while (cur != nullptr) {
      IntrusiveOrder* nxt = cur->next;
      pool_.destroy(cur);
      cur = nxt;
    }
  }
  bids_.clear();
  asks_.clear();
  index_.clear();
  orders_accepted_ = 0;
  orders_rejected_ = 0;
  cancels_succeeded_ = 0;
  cancels_missed_ = 0;
  trades_executed_ = 0;
  total_traded_qty_ = 0;
  trade_checksum_ = 0;
}

void PooledFlatMatchingEngine::record_trade(uint64_t aggressor, uint64_t resting, int64_t price,
                                            uint32_t qty, std::vector<Trade>* out_trades) {
  ++trades_executed_;
  total_traded_qty_ += qty;
  trade_checksum_ = mix_trade_checksum(trade_checksum_, aggressor, resting, price, qty);
  if (out_trades != nullptr) {
    out_trades->push_back(Trade{aggressor, resting, price, qty});
  }
}

PooledFlatMatchingEngine::LevelQueue* PooledFlatMatchingEngine::find_or_insert_level(
    std::vector<LevelQueue>& levels, int64_t price, bool is_bid) {
  // Bids sorted ascending (best = back()); asks sorted descending (best = back()).
  auto it = is_bid
                ? std::lower_bound(levels.begin(), levels.end(), price,
                                   [](const LevelQueue& lv, int64_t p) { return lv.price < p; })
                : std::lower_bound(levels.begin(), levels.end(), price,
                                   [](const LevelQueue& lv, int64_t p) { return lv.price > p; });
  if (it != levels.end() && it->price == price) {
    return &(*it);
  }
  it = levels.insert(it, LevelQueue{price, nullptr, nullptr});
  return &(*it);
}

void PooledFlatMatchingEngine::unlink_order(std::vector<LevelQueue>& levels, IntrusiveOrder* node,
                                            bool is_bid) {
  auto it = is_bid
                ? std::lower_bound(levels.begin(), levels.end(), node->price,
                                   [](const LevelQueue& lv, int64_t p) { return lv.price < p; })
                : std::lower_bound(levels.begin(), levels.end(), node->price,
                                   [](const LevelQueue& lv, int64_t p) { return lv.price > p; });
  if (it == levels.end() || it->price != node->price) return;

  if (node->prev != nullptr) {
    node->prev->next = node->next;
  } else {
    it->head = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  } else {
    it->tail = node->prev;
  }
  if (it->head == nullptr) {
    levels.erase(it);
  }
}

void PooledFlatMatchingEngine::process(const OrderCommand& cmd, std::vector<Trade>* out_trades) {
  switch (cmd.action) {
    case OrderAction::kAddLimit:
      handle_add(cmd, false, out_trades);
      break;
    case OrderAction::kAddMarket:
      handle_add(cmd, true, out_trades);
      break;
    case OrderAction::kCancel:
      handle_cancel(cmd);
      break;
    case OrderAction::kModifyQty:
      handle_modify(cmd);
      break;
  }
}

void PooledFlatMatchingEngine::handle_add(const OrderCommand& cmd, bool is_market,
                                          std::vector<Trade>* out_trades) {
  if (cmd.order_id == 0 || cmd.order_id == std::numeric_limits<uint64_t>::max() || cmd.qty == 0 ||
      (!is_market && cmd.price <= 0) || index_.find(cmd.order_id) != nullptr) {
    ++orders_rejected_;
    return;
  }

  // If resting may be needed and pool is full, reject cleanly without mutating state.
  if (!is_market && pool_.in_use() >= pool_.capacity()) {
    ++orders_rejected_;
    return;
  }

  ++orders_accepted_;
  uint32_t remaining = cmd.qty;

  if (cmd.side == Side::kBuy) {
    while (remaining > 0 && !asks_.empty()) {
      LevelQueue& best = asks_.back();
      if (!is_market && best.price > cmd.price) break;
      while (remaining > 0 && best.head != nullptr) {
        IntrusiveOrder* resting = best.head;
        const uint32_t fill = std::min(remaining, resting->qty);
        record_trade(cmd.order_id, resting->order_id, resting->price, fill, out_trades);
        remaining -= fill;
        resting->qty -= fill;
        if (resting->qty == 0) {
          best.head = resting->next;
          if (best.head != nullptr) {
            best.head->prev = nullptr;
          } else {
            best.tail = nullptr;
          }
          index_.erase(resting->order_id);
          pool_.destroy(resting);
        }
      }
      if (best.head == nullptr) {
        asks_.pop_back();
      }
    }
    if (!is_market && remaining > 0) {
      IntrusiveOrder* node = pool_.create();
      node->order_id = cmd.order_id;
      node->price = cmd.price;
      node->qty = remaining;
      node->side = Side::kBuy;
      LevelQueue* lvl = find_or_insert_level(bids_, cmd.price, true);
      node->prev = lvl->tail;
      node->next = nullptr;
      if (lvl->tail != nullptr) {
        lvl->tail->next = node;
      } else {
        lvl->head = node;
      }
      lvl->tail = node;
      index_.insert_or_assign(cmd.order_id, node);
    }
  } else {
    while (remaining > 0 && !bids_.empty()) {
      LevelQueue& best = bids_.back();
      if (!is_market && best.price < cmd.price) break;
      while (remaining > 0 && best.head != nullptr) {
        IntrusiveOrder* resting = best.head;
        const uint32_t fill = std::min(remaining, resting->qty);
        record_trade(cmd.order_id, resting->order_id, resting->price, fill, out_trades);
        remaining -= fill;
        resting->qty -= fill;
        if (resting->qty == 0) {
          best.head = resting->next;
          if (best.head != nullptr) {
            best.head->prev = nullptr;
          } else {
            best.tail = nullptr;
          }
          index_.erase(resting->order_id);
          pool_.destroy(resting);
        }
      }
      if (best.head == nullptr) {
        bids_.pop_back();
      }
    }
    if (!is_market && remaining > 0) {
      IntrusiveOrder* node = pool_.create();
      node->order_id = cmd.order_id;
      node->price = cmd.price;
      node->qty = remaining;
      node->side = Side::kSell;
      LevelQueue* lvl = find_or_insert_level(asks_, cmd.price, false);
      node->prev = lvl->tail;
      node->next = nullptr;
      if (lvl->tail != nullptr) {
        lvl->tail->next = node;
      } else {
        lvl->head = node;
      }
      lvl->tail = node;
      index_.insert_or_assign(cmd.order_id, node);
    }
  }
}

void PooledFlatMatchingEngine::handle_cancel(const OrderCommand& cmd) {
  IntrusiveOrder** found = index_.find(cmd.order_id);
  if (found == nullptr) {
    ++cancels_missed_;
    return;
  }
  IntrusiveOrder* node = *found;
  index_.erase(cmd.order_id);
  if (node->side == Side::kBuy) {
    unlink_order(bids_, node, true);
  } else {
    unlink_order(asks_, node, false);
  }
  pool_.destroy(node);
  ++cancels_succeeded_;
}

void PooledFlatMatchingEngine::handle_modify(const OrderCommand& cmd) {
  IntrusiveOrder** found = index_.find(cmd.order_id);
  if (found == nullptr) {
    ++orders_rejected_;
    return;
  }
  if (cmd.qty == 0) {
    handle_cancel(cmd);
    return;
  }
  IntrusiveOrder* node = *found;
  if (cmd.qty > node->qty) {
    ++orders_rejected_;
    return;
  }
  node->qty = cmd.qty;
  ++orders_accepted_;
}

EngineSummary PooledFlatMatchingEngine::summary() const noexcept {
  EngineSummary s;
  s.orders_accepted = orders_accepted_;
  s.orders_rejected = orders_rejected_;
  s.cancels_succeeded = cancels_succeeded_;
  s.cancels_missed = cancels_missed_;
  s.trades_executed = trades_executed_;
  s.total_traded_qty = total_traded_qty_;
  s.best_bid = bids_.empty() ? 0 : bids_.back().price;
  s.best_ask = asks_.empty() ? 0 : asks_.back().price;
  s.resting_orders = index_.size();
  s.trade_checksum = trade_checksum_;
  return s;
}

}  // namespace lpl
