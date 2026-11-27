#include "hft/runtime/canonical_book.hpp"

namespace hft::runtime {

using namespace contracts::v1;

namespace {

void hash_u64(std::uint64_t& h, std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    h ^= static_cast<std::uint8_t>((v >> (i * 8U)) & 0xFFU);
    h *= 1099511628211ULL;
  }
}

}  // namespace

CanonicalOrderBook::CanonicalOrderBook(std::int64_t stale_threshold_ns)
    : stale_threshold_ns_(stale_threshold_ns) {}

void CanonicalOrderBook::clear() noexcept {
  orders_.clear();
  bids_.clear();
  asks_.clear();
}

void CanonicalOrderBook::add_to_level(Side side, Price px, std::uint64_t qty) {
  if (qty == 0 || !px.valid()) return;
  if (side == Side::Buy) {
    auto& lvl = bids_[px.ticks];
    lvl.total_qty += qty;
    lvl.order_count += 1;
  } else {
    auto& lvl = asks_[px.ticks];
    lvl.total_qty += qty;
    lvl.order_count += 1;
  }
}

void CanonicalOrderBook::remove_from_level(Side side, Price px, std::uint64_t qty,
                                           bool decrement_count) {
  auto apply_map = [&](auto& mp) {
    auto it = mp.find(px.ticks);
    if (it == mp.end()) return;
    if (qty >= it->second.total_qty) {
      it->second.total_qty = 0;
    } else {
      it->second.total_qty -= qty;
    }
    if (decrement_count && it->second.order_count > 0) {
      it->second.order_count -= 1;
    }
    if (it->second.total_qty == 0 || it->second.order_count == 0) {
      mp.erase(it);
    }
  };
  if (side == Side::Buy) {
    apply_map(bids_);
  } else {
    apply_map(asks_);
  }
}

void CanonicalOrderBook::apply(const MarketDataEvent& md, SimTimeNs now_ns) {
  last_update_ns_ = now_ns;
  has_received_any_ = true;
  health_ = md.health;

  const std::uint64_t ref = md.feed_order_ref.value;
  switch (md.kind) {
    case MdUpdateKind::SnapshotClear:
    case MdUpdateKind::BookClear:
      clear();
      return;
    case MdUpdateKind::SnapshotLevel:
    case MdUpdateKind::Add: {
      if (!md.price.valid() || md.qty.lots == 0) return;
      auto it = orders_.find(ref);
      if (it != orders_.end()) {
        remove_from_level(it->second.side, it->second.price, it->second.qty.lots, true);
        orders_.erase(it);
      }
      orders_[ref] = RestingFeedOrder{md.side, md.price, md.qty};
      add_to_level(md.side, md.price, md.qty.lots);
      break;
    }
    case MdUpdateKind::Modify: {
      auto it = orders_.find(ref);
      if (it != orders_.end()) {
        remove_from_level(it->second.side, it->second.price, it->second.qty.lots, true);
        if (md.qty.lots == 0 || !md.price.valid()) {
          orders_.erase(it);
        } else {
          it->second.price = md.price;
          it->second.qty = md.qty;
          add_to_level(it->second.side, md.price, md.qty.lots);
        }
      } else if (md.price.valid() && md.qty.lots > 0) {
        orders_[ref] = RestingFeedOrder{md.side, md.price, md.qty};
        add_to_level(md.side, md.price, md.qty.lots);
      }
      break;
    }
    case MdUpdateKind::Cancel:
    case MdUpdateKind::Trade: {
      auto it = orders_.find(ref);
      if (it == orders_.end()) return;
      const std::uint64_t dec = md.qty.lots;
      if (dec >= it->second.qty.lots) {
        remove_from_level(it->second.side, it->second.price, it->second.qty.lots, true);
        orders_.erase(it);
      } else {
        it->second.qty.lots -= dec;
        remove_from_level(it->second.side, it->second.price, dec, false);
      }
      break;
    }
    case MdUpdateKind::Delete: {
      auto it = orders_.find(ref);
      if (it == orders_.end()) return;
      remove_from_level(it->second.side, it->second.price, it->second.qty.lots, true);
      orders_.erase(it);
      break;
    }
  }
}

bool CanonicalOrderBook::is_stale(SimTimeNs now_ns) const noexcept {
  if (!has_received_any_) return true;
  if (stale_threshold_ns_ <= 0) return false;
  return (now_ns.value - last_update_ns_.value) > stale_threshold_ns_;
}

bool CanonicalOrderBook::is_tradable(SimTimeNs now_ns) const noexcept {
  if (!has_received_any_) return false;
  if (health_ != FeedHealth::Healthy) return false;
  return !is_stale(now_ns);
}

TopOfBook CanonicalOrderBook::top_of_book(SimTimeNs now_ns) const noexcept {
  TopOfBook tob{};
  tob.last_update_ns = last_update_ns_;
  tob.health = is_stale(now_ns) && health_ == FeedHealth::Healthy ? FeedHealth::Stale : health_;
  if (!bids_.empty()) {
    const auto& [px, agg] = *bids_.begin();
    tob.bid = LevelView{Price{px}, Quantity{agg.total_qty}, agg.order_count};
  }
  if (!asks_.empty()) {
    const auto& [px, agg] = *asks_.begin();
    tob.ask = LevelView{Price{px}, Quantity{agg.total_qty}, agg.order_count};
  }
  return tob;
}

std::vector<LevelView> CanonicalOrderBook::depth(Side side, std::size_t max_levels) const {
  std::vector<LevelView> out;
  out.reserve(max_levels);
  if (side == Side::Buy) {
    for (const auto& [px, agg] : bids_) {
      if (out.size() >= max_levels) break;
      out.push_back(LevelView{Price{px}, Quantity{agg.total_qty}, agg.order_count});
    }
  } else {
    for (const auto& [px, agg] : asks_) {
      if (out.size() >= max_levels) break;
      out.push_back(LevelView{Price{px}, Quantity{agg.total_qty}, agg.order_count});
    }
  }
  return out;
}

std::uint64_t CanonicalOrderBook::state_hash() const noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  hash_u64(h, bids_.size());
  for (const auto& [px, agg] : bids_) {
    hash_u64(h, static_cast<std::uint64_t>(px));
    hash_u64(h, agg.total_qty);
    hash_u64(h, agg.order_count);
  }
  hash_u64(h, asks_.size());
  for (const auto& [px, agg] : asks_) {
    hash_u64(h, static_cast<std::uint64_t>(px));
    hash_u64(h, agg.total_qty);
    hash_u64(h, agg.order_count);
  }
  return h;
}

}  // namespace hft::runtime
