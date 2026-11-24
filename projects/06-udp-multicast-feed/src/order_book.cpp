#include "mdf/order_book.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace mdf {

namespace {

inline uint64_t fnv1a_mix_bytes(uint64_t h, const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint64_t>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

template <typename T>
inline uint64_t fnv1a_mix(uint64_t h, T value) noexcept {
  return fnv1a_mix_bytes(h, &value, sizeof(T));
}

}  // namespace

void InstrumentBook::add_to_level(Side side, int64_t price, uint32_t qty) {
  if (side == Side::kBid) {
    auto& lvl = bids_[price];
    lvl.price_ticks = price;
    lvl.total_qty += qty;
    lvl.order_count += 1;
  } else {
    auto& lvl = asks_[price];
    lvl.price_ticks = price;
    lvl.total_qty += qty;
    lvl.order_count += 1;
  }
}

void InstrumentBook::remove_from_level(Side side,
                                       int64_t price,
                                       uint32_t qty,
                                       bool decrement_order_count) {
  if (side == Side::kBid) {
    auto it = bids_.find(price);
    if (it == bids_.end()) return;
    it->second.total_qty = (it->second.total_qty > qty) ? (it->second.total_qty - qty) : 0;
    if (decrement_order_count && it->second.order_count > 0) {
      it->second.order_count -= 1;
    }
    if (it->second.total_qty == 0 || it->second.order_count == 0) {
      bids_.erase(it);
    }
  } else {
    auto it = asks_.find(price);
    if (it == asks_.end()) return;
    it->second.total_qty = (it->second.total_qty > qty) ? (it->second.total_qty - qty) : 0;
    if (decrement_order_count && it->second.order_count > 0) {
      it->second.order_count -= 1;
    }
    if (it->second.total_qty == 0 || it->second.order_count == 0) {
      asks_.erase(it);
    }
  }
}

void InstrumentBook::add_order(const Order& order) {
  if (order.qty == 0) return;
  auto it = orders_.find(order.order_id);
  if (it != orders_.end()) {
    remove_from_level(it->second.side, it->second.price_ticks, it->second.qty, true);
    it->second = order;
  } else {
    orders_.emplace(order.order_id, order);
  }
  add_to_level(order.side, order.price_ticks, order.qty);
}

void InstrumentBook::modify_order(uint64_t order_id, int64_t new_price_ticks, uint32_t new_qty) {
  auto it = orders_.find(order_id);
  if (it == orders_.end()) return;
  Order& existing = it->second;
  if (new_qty == 0) {
    remove_from_level(existing.side, existing.price_ticks, existing.qty, true);
    orders_.erase(it);
    return;
  }
  if (existing.price_ticks == new_price_ticks) {
    if (new_qty > existing.qty) {
      const uint32_t diff = new_qty - existing.qty;
      if (existing.side == Side::kBid) {
        bids_[existing.price_ticks].total_qty += diff;
      } else {
        asks_[existing.price_ticks].total_qty += diff;
      }
    } else if (new_qty < existing.qty) {
      remove_from_level(existing.side, existing.price_ticks, existing.qty - new_qty, false);
    }
    existing.qty = new_qty;
  } else {
    remove_from_level(existing.side, existing.price_ticks, existing.qty, true);
    existing.price_ticks = new_price_ticks;
    existing.qty = new_qty;
    add_to_level(existing.side, existing.price_ticks, existing.qty);
  }
}

void InstrumentBook::cancel_order(uint64_t order_id, uint32_t cancel_qty) {
  auto it = orders_.find(order_id);
  if (it == orders_.end()) return;
  Order& existing = it->second;
  if (cancel_qty == 0 || cancel_qty >= existing.qty) {
    remove_from_level(existing.side, existing.price_ticks, existing.qty, true);
    orders_.erase(it);
  } else {
    remove_from_level(existing.side, existing.price_ticks, cancel_qty, false);
    existing.qty -= cancel_qty;
  }
}

void InstrumentBook::apply_trade(uint64_t resting_order_id,
                                 int64_t exec_price_ticks,
                                 uint32_t exec_qty) {
  if (exec_qty == 0) return;
  stats_.last_trade_price_ticks = exec_price_ticks;
  stats_.last_trade_qty = exec_qty;
  stats_.trade_count += 1;
  stats_.total_volume += exec_qty;

  auto it = orders_.find(resting_order_id);
  if (it == orders_.end()) return;
  Order& existing = it->second;
  if (exec_qty >= existing.qty) {
    remove_from_level(existing.side, existing.price_ticks, existing.qty, true);
    orders_.erase(it);
  } else {
    remove_from_level(existing.side, existing.price_ticks, exec_qty, false);
    existing.qty -= exec_qty;
  }
}

void InstrumentBook::clear() {
  orders_.clear();
  bids_.clear();
  asks_.clear();
  stats_ = InstrumentStats{};
}

std::optional<PriceLevel> InstrumentBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->second;
}

std::optional<PriceLevel> InstrumentBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->second;
}

bool InstrumentBook::operator==(const InstrumentBook& other) const {
  return orders_ == other.orders_ && bids_ == other.bids_ && asks_ == other.asks_ &&
         stats_ == other.stats_;
}

void ChannelMarketState::apply_event(const MarketEvent& ev) {
  ++total_events_applied_;
  switch (ev.type) {
    case EventType::kAddOrder: {
      Order o{};
      o.instrument_id = ev.instrument_id;
      o.order_id = ev.order_id;
      o.side = ev.side;
      o.price_ticks = ev.price_ticks;
      o.qty = ev.qty;
      instruments_[ev.instrument_id].add_order(o);
      break;
    }
    case EventType::kModifyOrder:
      instruments_[ev.instrument_id].modify_order(ev.order_id, ev.price_ticks, ev.qty);
      break;
    case EventType::kCancelOrder:
      instruments_[ev.instrument_id].cancel_order(ev.order_id, ev.qty);
      break;
    case EventType::kTrade:
      instruments_[ev.instrument_id].apply_trade(ev.order_id, ev.price_ticks, ev.qty);
      break;
    case EventType::kClearBook:
      if (ev.instrument_id == 0) {
        instruments_.clear();
      } else {
        auto it = instruments_.find(ev.instrument_id);
        if (it != instruments_.end()) {
          it->second.clear();
        }
      }
      break;
  }
}

void ChannelMarketState::apply_packet(uint64_t seq_num, std::span<const MarketEvent> events) {
  for (const auto& ev : events) {
    apply_event(ev);
  }
  last_applied_seq_ = seq_num;
}

SnapshotData ChannelMarketState::export_snapshot(uint64_t checkpoint_id) const {
  SnapshotData snap{};
  snap.channel_id = channel_id_;
  snap.as_of_seq = last_applied_seq_;
  snap.checkpoint_id = checkpoint_id;

  for (const auto& [inst_id, book] : instruments_) {
    std::vector<Order> sorted_orders;
    sorted_orders.reserve(book.orders().size());
    for (const auto& [oid, o] : book.orders()) {
      (void)oid;
      sorted_orders.push_back(o);
    }
    std::sort(sorted_orders.begin(), sorted_orders.end(),
              [](const Order& a, const Order& b) { return a.order_id < b.order_id; });
    for (const auto& o : sorted_orders) {
      snap.orders.push_back(SnapshotOrder{o.instrument_id, o.order_id, o.side, o.price_ticks,
                                          o.qty});
    }
    const auto& st = book.stats();
    if (st.trade_count > 0 || !book.orders().empty()) {
      snap.instrument_stats.push_back(SnapshotInstrumentStats{
          inst_id, st.last_trade_price_ticks, st.last_trade_qty, st.trade_count, st.total_volume});
    }
  }
  return snap;
}

void ChannelMarketState::load_snapshot(const SnapshotData& snap) {
  instruments_.clear();
  channel_id_ = snap.channel_id;
  last_applied_seq_ = snap.as_of_seq;
  for (const auto& o : snap.orders) {
    instruments_[o.instrument_id].add_order(
        Order{o.instrument_id, o.order_id, o.side, o.price_ticks, o.qty});
  }
  for (const auto& st : snap.instrument_stats) {
    instruments_[st.instrument_id].set_stats(InstrumentStats{
        st.last_trade_price_ticks, st.last_trade_qty, st.trade_count, st.total_volume});
  }
}

void ChannelMarketState::clear() {
  instruments_.clear();
  last_applied_seq_ = 0;
  total_events_applied_ = 0;
}

const InstrumentBook* ChannelMarketState::find_instrument(uint32_t instrument_id) const {
  auto it = instruments_.find(instrument_id);
  if (it == instruments_.end()) return nullptr;
  return &it->second;
}

std::size_t ChannelMarketState::total_active_orders() const noexcept {
  std::size_t count = 0;
  for (const auto& [inst_id, book] : instruments_) {
    (void)inst_id;
    count += book.order_count();
  }
  return count;
}

uint64_t ChannelMarketState::state_hash() const {
  uint64_t h = 14695981039346656037ULL;
  h = fnv1a_mix(h, channel_id_);
  h = fnv1a_mix(h, last_applied_seq_);

  for (const auto& [inst_id, book] : instruments_) {
    // Skip completely empty instruments with zero stats so cleared vs never-created match.
    if (book.order_count() == 0 && book.stats().trade_count == 0) {
      continue;
    }
    h = fnv1a_mix(h, inst_id);
    h = fnv1a_mix(h, book.stats().last_trade_price_ticks);
    h = fnv1a_mix(h, book.stats().last_trade_qty);
    h = fnv1a_mix(h, book.stats().trade_count);
    h = fnv1a_mix(h, book.stats().total_volume);

    std::vector<Order> sorted_orders;
    sorted_orders.reserve(book.orders().size());
    for (const auto& [oid, o] : book.orders()) {
      (void)oid;
      sorted_orders.push_back(o);
    }
    std::sort(sorted_orders.begin(), sorted_orders.end(),
              [](const Order& a, const Order& b) { return a.order_id < b.order_id; });
    for (const auto& o : sorted_orders) {
      h = fnv1a_mix(h, o.order_id);
      h = fnv1a_mix(h, static_cast<uint8_t>(o.side));
      h = fnv1a_mix(h, o.price_ticks);
      h = fnv1a_mix(h, o.qty);
    }
    for (const auto& [px, lvl] : book.bids()) {
      (void)px;
      h = fnv1a_mix(h, lvl.price_ticks);
      h = fnv1a_mix(h, lvl.total_qty);
      h = fnv1a_mix(h, lvl.order_count);
    }
    for (const auto& [px, lvl] : book.asks()) {
      (void)px;
      h = fnv1a_mix(h, lvl.price_ticks);
      h = fnv1a_mix(h, lvl.total_qty);
      h = fnv1a_mix(h, lvl.order_count);
    }
  }
  return h;
}

bool ChannelMarketState::operator==(const ChannelMarketState& other) const {
  if (channel_id_ != other.channel_id_ || last_applied_seq_ != other.last_applied_seq_) {
    return false;
  }
  // Compare non-empty instruments.
  auto collect_non_empty = [](const std::map<uint32_t, InstrumentBook>& src) {
    std::map<uint32_t, const InstrumentBook*> out;
    for (const auto& [id, book] : src) {
      if (book.order_count() > 0 || book.stats().trade_count > 0) {
        out.emplace(id, &book);
      }
    }
    return out;
  };
  const auto lhs = collect_non_empty(instruments_);
  const auto rhs = collect_non_empty(other.instruments_);
  if (lhs.size() != rhs.size()) return false;
  for (const auto& [id, lbook] : lhs) {
    auto it = rhs.find(id);
    if (it == rhs.end() || !(*lbook == *(it->second))) {
      return false;
    }
  }
  return true;
}

std::string ChannelMarketState::diff(const ChannelMarketState& other) const {
  if (*this == other) return {};
  std::ostringstream oss;
  if (channel_id_ != other.channel_id_) {
    oss << "channel_id mismatch: " << channel_id_ << " vs " << other.channel_id_ << "\n";
  }
  if (last_applied_seq_ != other.last_applied_seq_) {
    oss << "last_applied_seq mismatch: " << last_applied_seq_ << " vs " << other.last_applied_seq_
        << "\n";
  }
  for (const auto& [id, book] : instruments_) {
    auto it = other.instruments_.find(id);
    if (it == other.instruments_.end()) {
      if (book.order_count() > 0 || book.stats().trade_count > 0) {
        oss << "instrument " << id << " present only on LHS (orders=" << book.order_count()
            << ")\n";
      }
    } else if (!(book == it->second)) {
      oss << "instrument " << id << " differs: LHS orders=" << book.order_count()
          << " trades=" << book.stats().trade_count
          << " vs RHS orders=" << it->second.order_count()
          << " trades=" << it->second.stats().trade_count << "\n";
    }
  }
  for (const auto& [id, book] : other.instruments_) {
    if (instruments_.find(id) == instruments_.end() &&
        (book.order_count() > 0 || book.stats().trade_count > 0)) {
      oss << "instrument " << id << " present only on RHS (orders=" << book.order_count() << ")\n";
    }
  }
  return oss.str();
}

}  // namespace mdf
