#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "xarb/types.hpp"

namespace xarb {

struct VwapFillResult {
  QtyLots filled_lots{0};
  double vwap_ticks{0.0};       // volume-weighted average price in ticks
  PriceTicks worst_price{0};    // worst level touched
  PriceTicks best_price{0};     // top-of-book price
  size_t levels_consumed{0};
  bool full_fill{false};
};

struct CrossExecutableResult {
  QtyLots executable_lots{0};
  double buy_vwap_ticks{0.0};
  double sell_vwap_ticks{0.0};
  PriceTicks buy_worst_ticks{0};
  PriceTicks sell_worst_ticks{0};
  PriceTicks buy_top_ticks{0};
  PriceTicks sell_top_ticks{0};
  size_t buy_levels_used{0};
  size_t sell_levels_used{0};
  double gross_edge_bps{0.0};        // top-of-book gross spread in bps
  double net_vwap_edge_bps{0.0};     // depth-aware VWAP edge after taker fees in bps
};

// Cache-friendly flat sorted-vector L2 order book.
// Bids are stored descending (index 0 = best bid); asks are stored ascending (index 0 = best ask).
// Optional max_depth bounds memory and update cost for top-N books (0 = unlimited).
class FlatOrderBook {
 public:
  explicit FlatOrderBook(size_t max_depth = 0) : max_depth_(max_depth) {
    if (max_depth > 0) {
      bids_.reserve(max_depth + 4);
      asks_.reserve(max_depth + 4);
    } else {
      bids_.reserve(128);
      asks_.reserve(128);
    }
  }

  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  void apply_snapshot(const std::vector<PriceLevel>& bids, const std::vector<PriceLevel>& asks) {
    bids_.clear();
    asks_.clear();
    for (const auto& lvl : bids) {
      if (lvl.qty > 0 && lvl.price > 0) bids_.push_back(lvl);
    }
    for (const auto& lvl : asks) {
      if (lvl.qty > 0 && lvl.price > 0) asks_.push_back(lvl);
    }
    std::sort(bids_.begin(), bids_.end(), [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    std::sort(asks_.begin(), asks_.end(), [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });
    dedup_keep_last(bids_);
    dedup_keep_last(asks_);
    trim_depth();
  }

  void apply_delta(Side side, PriceTicks price, QtyLots qty) {
    if (price <= 0) return;
    if (side == Side::Bid) {
      auto it = std::lower_bound(bids_.begin(), bids_.end(), price,
                                 [](const PriceLevel& lvl, PriceTicks p) { return lvl.price > p; });
      if (it != bids_.end() && it->price == price) {
        if (qty <= 0) {
          bids_.erase(it);
        } else {
          it->qty = qty;
        }
      } else if (qty > 0) {
        bids_.insert(it, PriceLevel{price, qty});
        trim_side(bids_);
      }
    } else {
      auto it = std::lower_bound(asks_.begin(), asks_.end(), price,
                                 [](const PriceLevel& lvl, PriceTicks p) { return lvl.price < p; });
      if (it != asks_.end() && it->price == price) {
        if (qty <= 0) {
          asks_.erase(it);
        } else {
          it->qty = qty;
        }
      } else if (qty > 0) {
        asks_.insert(it, PriceLevel{price, qty});
        trim_side(asks_);
      }
    }
  }

  std::optional<PriceLevel> best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.front();
  }

  std::optional<PriceLevel> best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.front();
  }

  bool is_crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.front().price >= asks_.front().price;
  }

  bool empty() const noexcept { return bids_.empty() && asks_.empty(); }
  size_t bid_levels() const noexcept { return bids_.size(); }
  size_t ask_levels() const noexcept { return asks_.size(); }
  const std::vector<PriceLevel>& bids() const noexcept { return bids_; }
  const std::vector<PriceLevel>& asks() const noexcept { return asks_; }

  // Walk asks to buy target_lots (optional limit_price_ticks: 0 = market/no limit).
  VwapFillResult walk_asks(QtyLots target_lots, PriceTicks max_price_ticks = 0) const noexcept {
    return walk_side(asks_, target_lots, max_price_ticks, /*is_ask=*/true);
  }

  // Walk bids to sell target_lots (optional min_price_ticks: 0 = market/no limit).
  VwapFillResult walk_bids(QtyLots target_lots, PriceTicks min_price_ticks = 0) const noexcept {
    return walk_side(bids_, target_lots, min_price_ticks, /*is_ask=*/false);
  }

 private:
  static void dedup_keep_last(std::vector<PriceLevel>& v) {
    if (v.size() < 2) return;
    size_t write = 0;
    for (size_t read = 1; read < v.size(); ++read) {
      if (v[read].price == v[write].price) {
        v[write].qty = v[read].qty;
      } else {
        v[++write] = v[read];
      }
    }
    v.resize(write + 1);
  }

  void trim_side(std::vector<PriceLevel>& v) {
    if (max_depth_ > 0 && v.size() > max_depth_) {
      v.resize(max_depth_);
    }
  }

  void trim_depth() {
    trim_side(bids_);
    trim_side(asks_);
  }

  static VwapFillResult walk_side(const std::vector<PriceLevel>& levels, QtyLots target_lots,
                                  PriceTicks limit_price, bool is_ask) noexcept {
    VwapFillResult res;
    if (target_lots <= 0 || levels.empty()) return res;
    res.best_price = levels.front().price;
    QtyLots remaining = target_lots;
    long double weighted_sum = 0.0L;

    for (const auto& lvl : levels) {
      if (limit_price > 0) {
        if (is_ask && lvl.price > limit_price) break;
        if (!is_ask && lvl.price < limit_price) break;
      }
      const QtyLots take = std::min(remaining, lvl.qty);
      weighted_sum += static_cast<long double>(take) * static_cast<long double>(lvl.price);
      res.filled_lots += take;
      res.worst_price = lvl.price;
      res.levels_consumed += 1;
      remaining -= take;
      if (remaining == 0) {
        res.full_fill = true;
        break;
      }
    }

    if (res.filled_lots > 0) {
      res.vwap_ticks = static_cast<double>(weighted_sum / static_cast<long double>(res.filled_lots));
    }
    return res;
  }

  size_t max_depth_{0};
  std::vector<PriceLevel> bids_;
  std::vector<PriceLevel> asks_;
};

// Reference std::map-based L2 order book for differential verification and performance benchmarking.
class MapOrderBook {
 public:
  explicit MapOrderBook(size_t max_depth = 0) : max_depth_(max_depth) {}

  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  void apply_snapshot(const std::vector<PriceLevel>& bids, const std::vector<PriceLevel>& asks) {
    bids_.clear();
    asks_.clear();
    for (const auto& lvl : bids) {
      if (lvl.qty > 0 && lvl.price > 0) bids_[lvl.price] = lvl.qty;
    }
    for (const auto& lvl : asks) {
      if (lvl.qty > 0 && lvl.price > 0) asks_[lvl.price] = lvl.qty;
    }
    trim_depth();
  }

  void apply_delta(Side side, PriceTicks price, QtyLots qty) {
    if (price <= 0) return;
    if (side == Side::Bid) {
      if (qty <= 0) {
        bids_.erase(price);
      } else {
        bids_[price] = qty;
        while (max_depth_ > 0 && bids_.size() > max_depth_) {
          bids_.erase(std::prev(bids_.end()));
        }
      }
    } else {
      if (qty <= 0) {
        asks_.erase(price);
      } else {
        asks_[price] = qty;
        while (max_depth_ > 0 && asks_.size() > max_depth_) {
          asks_.erase(std::prev(asks_.end()));
        }
      }
    }
  }

  std::optional<PriceLevel> best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    const auto& [p, q] = *bids_.begin();
    return PriceLevel{p, q};
  }

  std::optional<PriceLevel> best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    const auto& [p, q] = *asks_.begin();
    return PriceLevel{p, q};
  }

  bool is_crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
  }

  size_t bid_levels() const noexcept { return bids_.size(); }
  size_t ask_levels() const noexcept { return asks_.size(); }

  VwapFillResult walk_asks(QtyLots target_lots, PriceTicks max_price_ticks = 0) const noexcept {
    VwapFillResult res;
    if (target_lots <= 0 || asks_.empty()) return res;
    res.best_price = asks_.begin()->first;
    QtyLots remaining = target_lots;
    long double weighted_sum = 0.0L;
    for (const auto& [p, q] : asks_) {
      if (max_price_ticks > 0 && p > max_price_ticks) break;
      const QtyLots take = std::min(remaining, q);
      weighted_sum += static_cast<long double>(take) * static_cast<long double>(p);
      res.filled_lots += take;
      res.worst_price = p;
      res.levels_consumed += 1;
      remaining -= take;
      if (remaining == 0) {
        res.full_fill = true;
        break;
      }
    }
    if (res.filled_lots > 0) {
      res.vwap_ticks = static_cast<double>(weighted_sum / static_cast<long double>(res.filled_lots));
    }
    return res;
  }

  VwapFillResult walk_bids(QtyLots target_lots, PriceTicks min_price_ticks = 0) const noexcept {
    VwapFillResult res;
    if (target_lots <= 0 || bids_.empty()) return res;
    res.best_price = bids_.begin()->first;
    QtyLots remaining = target_lots;
    long double weighted_sum = 0.0L;
    for (const auto& [p, q] : bids_) {
      if (min_price_ticks > 0 && p < min_price_ticks) break;
      const QtyLots take = std::min(remaining, q);
      weighted_sum += static_cast<long double>(take) * static_cast<long double>(p);
      res.filled_lots += take;
      res.worst_price = p;
      res.levels_consumed += 1;
      remaining -= take;
      if (remaining == 0) {
        res.full_fill = true;
        break;
      }
    }
    if (res.filled_lots > 0) {
      res.vwap_ticks = static_cast<double>(weighted_sum / static_cast<long double>(res.filled_lots));
    }
    return res;
  }

 private:
  void trim_depth() {
    while (max_depth_ > 0 && bids_.size() > max_depth_) bids_.erase(std::prev(bids_.end()));
    while (max_depth_ > 0 && asks_.size() > max_depth_) asks_.erase(std::prev(asks_.end()));
  }

  size_t max_depth_{0};
  std::map<PriceTicks, QtyLots, std::greater<PriceTicks>> bids_;
  std::map<PriceTicks, QtyLots, std::less<PriceTicks>> asks_;
};

// Depth-aware marginal executable sizing across two books:
// Walks buy_book.asks() and sell_book.bids() simultaneously, accumulating volume while the
// marginal level pair still satisfies:
//   sell_bid * (1 - sell_fee) > buy_ask * (1 + buy_fee) * (1 + min_marginal_edge_bps / 10000)
// capped by max_lots.
inline CrossExecutableResult compute_executable_cross(const FlatOrderBook& buy_book,
                                                      const FlatOrderBook& sell_book,
                                                      double buy_fee_bps,
                                                      double sell_fee_bps,
                                                      double min_marginal_edge_bps,
                                                      QtyLots max_lots) noexcept {
  CrossExecutableResult out;
  const auto& asks = buy_book.asks();
  const auto& bids = sell_book.bids();
  if (asks.empty() || bids.empty() || max_lots <= 0) return out;

  out.buy_top_ticks = asks.front().price;
  out.sell_top_ticks = bids.front().price;
  const double mid_top = 0.5 * (static_cast<double>(out.buy_top_ticks) + static_cast<double>(out.sell_top_ticks));
  if (mid_top > 0.0) {
    out.gross_edge_bps =
        (static_cast<double>(out.sell_top_ticks - out.buy_top_ticks) / static_cast<double>(out.buy_top_ticks)) *
        10'000.0;
  }

  const double buy_mult = 1.0 + buy_fee_bps / 10'000.0;
  const double sell_mult = 1.0 - sell_fee_bps / 10'000.0;
  const double edge_mult = 1.0 + min_marginal_edge_bps / 10'000.0;

  size_t ai = 0;
  size_t bi = 0;
  QtyLots a_rem = asks[0].qty;
  QtyLots b_rem = bids[0].qty;
  QtyLots cap_rem = max_lots;

  long double buy_weighted = 0.0L;
  long double sell_weighted = 0.0L;

  while (ai < asks.size() && bi < bids.size() && cap_rem > 0) {
    const double eff_buy = static_cast<double>(asks[ai].price) * buy_mult;
    const double eff_sell = static_cast<double>(bids[bi].price) * sell_mult;
    if (eff_sell < eff_buy * edge_mult) {
      break;
    }

    const QtyLots chunk = std::min({a_rem, b_rem, cap_rem});
    if (chunk <= 0) break;

    buy_weighted += static_cast<long double>(chunk) * static_cast<long double>(asks[ai].price);
    sell_weighted += static_cast<long double>(chunk) * static_cast<long double>(bids[bi].price);
    out.executable_lots += chunk;
    out.buy_worst_ticks = asks[ai].price;
    out.sell_worst_ticks = bids[bi].price;
    out.buy_levels_used = ai + 1;
    out.sell_levels_used = bi + 1;

    a_rem -= chunk;
    b_rem -= chunk;
    cap_rem -= chunk;

    if (a_rem == 0) {
      ++ai;
      if (ai < asks.size()) a_rem = asks[ai].qty;
    }
    if (b_rem == 0) {
      ++bi;
      if (bi < bids.size()) b_rem = bids[bi].qty;
    }
  }

  if (out.executable_lots > 0) {
    out.buy_vwap_ticks = static_cast<double>(buy_weighted / static_cast<long double>(out.executable_lots));
    out.sell_vwap_ticks = static_cast<double>(sell_weighted / static_cast<long double>(out.executable_lots));
    const double eff_buy_vwap = out.buy_vwap_ticks * buy_mult;
    const double eff_sell_vwap = out.sell_vwap_ticks * sell_mult;
    if (eff_buy_vwap > 0.0) {
      out.net_vwap_edge_bps = ((eff_sell_vwap - eff_buy_vwap) / eff_buy_vwap) * 10'000.0;
    }
  }

  return out;
}

}  // namespace xarb
