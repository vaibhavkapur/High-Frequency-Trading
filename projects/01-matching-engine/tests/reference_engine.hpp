// ReferenceEngine: the differential-testing oracle.
//
// Deliberately naive and written independently of me::Engine and both books: each
// symbol is one flat vector of resting orders kept in arrival order, and every
// operation is a linear scan. It shares only the wire structs with production
// code. Anything the production engine reports must match this, report for report.
#pragma once

#include <cstddef>
#include <vector>

#include "me/engine.hpp"
#include "me/protocol.hpp"

namespace me::test {

class ReferenceEngine {
 public:
  explicit ReferenceEngine(const EngineConfig& config) : config_(config), books_(config.symbols.size()) {}

  std::vector<Report> process(const Command& c) {
    out_.clear();
    const bool known = c.type == CommandType::New || c.type == CommandType::Cancel || c.type == CommandType::Reduce ||
                       c.type == CommandType::Replace;
    if (!known) {
      reject(c, RejectReason::UnknownCommand);
    } else if (c.symbol >= books_.size()) {
      reject(c, RejectReason::UnknownSymbol);
    } else if (c.type == CommandType::New) {
      do_new(c);
    } else if (c.type == CommandType::Cancel) {
      do_cancel(c);
    } else if (c.type == CommandType::Reduce) {
      do_reduce(c);
    } else {
      do_replace(c);
    }
    return out_;
  }

  // Aggregated levels of one side, best price first.
  std::vector<LevelView> depth(SymbolId symbol, Side side) const {
    std::vector<LevelView> levels;
    for (const Resting& o : books_[symbol]) {
      if (o.side != side) continue;
      bool merged = false;
      for (LevelView& l : levels) {
        if (l.price == o.price) {
          l.qty += o.qty;
          ++l.orders;
          merged = true;
        }
      }
      if (!merged) levels.push_back(LevelView{o.price, o.qty, 1});
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {  // selection sort, best first
      for (std::size_t j = i + 1; j < levels.size(); ++j) {
        const bool better = side == Side::Buy ? levels[j].price > levels[i].price : levels[j].price < levels[i].price;
        if (better) std::swap(levels[i], levels[j]);
      }
    }
    return levels;
  }

  std::size_t resting(SymbolId symbol) const { return books_[symbol].size(); }

 private:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
  };

  Report& emit(ReportType type, const Command& c) {
    Report r;
    r.seq = ++seq_;
    r.symbol = c.symbol;
    r.type = type;
    out_.push_back(r);
    return out_.back();
  }

  void reject(const Command& c, RejectReason reason) {
    Report& r = emit(ReportType::Rejected, c);
    r.order_id = c.order_id;
    r.other_id = c.new_order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.side = c.side;
    r.reason = reason;
  }

  Resting* find(SymbolId symbol, OrderId id) {
    for (Resting& o : books_[symbol]) {
      if (o.id == id) return &o;
    }
    return nullptr;
  }

  void erase(SymbolId symbol, OrderId id) {
    std::vector<Resting>& book = books_[symbol];
    for (std::size_t i = 0; i < book.size(); ++i) {
      if (book[i].id == id) {
        book.erase(book.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  RejectReason check(const Command& c) const {
    const SymbolConfig& s = config_.symbols[c.symbol];
    if (c.price <= 0 || c.price > s.max_price) return RejectReason::InvalidPrice;
    if (c.price % s.tick_size != 0) return RejectReason::InvalidTick;
    if (c.qty <= 0 || c.qty > s.max_qty || c.qty % s.lot_size != 0) return RejectReason::InvalidQuantity;
    return RejectReason::None;
  }

  // Trades `qty` of an incoming order against the book, then rests what is left.
  void enter(const Command& c, OrderId id, Side side, Price price, Qty qty) {
    std::vector<Resting>& book = books_[c.symbol];
    while (qty > 0) {
      // Best opposite order: best price, and the earliest arrival at that price
      // (the vector is in arrival order, so the first one found wins ties).
      Resting* best = nullptr;
      for (Resting& o : book) {
        if (o.side == side) continue;
        const bool crosses = side == Side::Buy ? o.price <= price : o.price >= price;
        if (!crosses) continue;
        const bool better = best == nullptr || (side == Side::Buy ? o.price < best->price : o.price > best->price);
        if (better) best = &o;
      }
      if (best == nullptr) break;
      const Qty fill = qty < best->qty ? qty : best->qty;
      qty -= fill;
      best->qty -= fill;
      Report& r = emit(ReportType::Trade, c);
      r.order_id = id;
      r.other_id = best->id;
      r.price = best->price;
      r.qty = fill;
      r.leaves = qty;
      r.other_leaves = best->qty;
      r.side = side;
      if (best->qty == 0) erase(c.symbol, best->id);
    }
    if (qty > 0) book.push_back(Resting{id, side, price, qty});
  }

  void do_new(const Command& c) {
    if (c.side != Side::Buy && c.side != Side::Sell) return reject(c, RejectReason::InvalidSide);
    if (const RejectReason why = check(c); why != RejectReason::None) return reject(c, why);
    if (find(c.symbol, c.order_id) != nullptr) return reject(c, RejectReason::DuplicateOrderId);
    if (books_[c.symbol].size() >= config_.max_orders_per_book) return reject(c, RejectReason::BookFull);
    Report& r = emit(ReportType::Accepted, c);
    r.order_id = c.order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.leaves = c.qty;
    r.side = c.side;
    enter(c, c.order_id, c.side, c.price, c.qty);
  }

  void do_cancel(const Command& c) {
    const Resting* o = find(c.symbol, c.order_id);
    if (o == nullptr) return reject(c, RejectReason::UnknownOrderId);
    Report& r = emit(ReportType::Canceled, c);
    r.order_id = o->id;
    r.price = o->price;
    r.qty = o->qty;
    r.side = o->side;
    erase(c.symbol, c.order_id);
  }

  void do_reduce(const Command& c) {
    Resting* o = find(c.symbol, c.order_id);
    if (o == nullptr) return reject(c, RejectReason::UnknownOrderId);
    if (c.qty <= 0 || c.qty >= o->qty || c.qty % config_.symbols[c.symbol].lot_size != 0) {
      return reject(c, RejectReason::InvalidQuantity);
    }
    Report& r = emit(ReportType::Reduced, c);
    r.order_id = o->id;
    r.price = o->price;
    r.qty = o->qty - c.qty;
    r.leaves = c.qty;
    r.side = o->side;
    o->qty = c.qty;  // stays where it is in the vector: priority kept
  }

  void do_replace(const Command& c) {
    const Resting* o = find(c.symbol, c.order_id);
    if (o == nullptr) return reject(c, RejectReason::UnknownOrderId);
    if (const RejectReason why = check(c); why != RejectReason::None) return reject(c, why);
    if (c.new_order_id != c.order_id && find(c.symbol, c.new_order_id) != nullptr) {
      return reject(c, RejectReason::DuplicateOrderId);
    }
    const Side side = o->side;
    Report& r = emit(ReportType::Replaced, c);
    r.order_id = c.order_id;
    r.other_id = c.new_order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.leaves = c.qty;
    r.other_leaves = o->qty;
    r.side = side;
    erase(c.symbol, c.order_id);
    enter(c, c.new_order_id, side, c.price, c.qty);  // re-enters at the back: priority lost
  }

  EngineConfig config_;
  std::vector<std::vector<Resting>> books_;
  std::vector<Report> out_;
  Seq seq_ = 0;
};

}  // namespace me::test
