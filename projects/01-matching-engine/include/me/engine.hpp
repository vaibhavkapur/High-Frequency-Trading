// Engine: command validator + single-writer matching core over N independent books.
//
// The engine is a plain single-threaded state machine: process() takes one
// command, mutates at most one book, and synchronously hands every resulting
// report to the caller's sink. It does no I/O, no logging and no locking; a
// deployment runs one engine per thread and keeps networking and persistence on
// the other side of the sink. Identical command sequences always produce
// identical report sequences.
#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "me/protocol.hpp"
#include "me/types.hpp"

namespace me {

struct SymbolConfig {
  Price tick_size = 1;                  // valid prices are positive multiples of this
  Qty lot_size = 1;                     // valid quantities are positive multiples of this
  Price max_price = 1'000'000'000'000;  // inclusive
  Qty max_qty = 1'000'000'000;          // inclusive, per order
};

struct EngineConfig {
  std::vector<SymbolConfig> symbols;             // SymbolId is the index into this vector
  std::uint32_t max_orders_per_book = 1u << 20;  // bound on resting orders per symbol
};

struct EngineStats {
  std::uint64_t commands = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t trades = 0;
  std::uint64_t canceled = 0;
  std::uint64_t reduced = 0;
  std::uint64_t replaced = 0;
  Qty traded_qty = 0;

  friend bool operator==(const EngineStats&, const EngineStats&) = default;
};

struct TopOfBook {
  std::optional<LevelView> bid;
  std::optional<LevelView> ask;

  friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

template <class Book>
class Engine {
 public:
  explicit Engine(EngineConfig config) : config_(std::move(config)) {
    if (config_.max_orders_per_book == 0) {
      throw std::invalid_argument("max_orders_per_book must be positive");
    }
    for (const SymbolConfig& s : config_.symbols) {
      if (s.tick_size <= 0 || s.lot_size <= 0 || s.max_price <= 0 || s.max_qty <= 0) {
        throw std::invalid_argument("symbol limits must be positive");
      }
      // A level's aggregate quantity must not overflow even if every order sits on it.
      if (s.max_qty > std::numeric_limits<Qty>::max() / config_.max_orders_per_book) {
        throw std::invalid_argument("max_qty * max_orders_per_book overflows Qty");
      }
    }
    books_.reserve(config_.symbols.size());
    for (std::size_t i = 0; i < config_.symbols.size(); ++i) {
      books_.emplace_back(config_.max_orders_per_book);
    }
  }

  // Applies one command. `sink(const Report&)` is called once per report, in
  // sequence order, before process() returns. Every command yields at least one report.
  template <class Sink>
  void process(const Command& c, Sink&& sink) {
    ++stats_.commands;
    switch (c.type) {
      case CommandType::New:
      case CommandType::Cancel:
      case CommandType::Reduce:
      case CommandType::Replace:
        break;
      default:
        return reject(c, RejectReason::UnknownCommand, sink);
    }
    if (c.symbol >= books_.size()) return reject(c, RejectReason::UnknownSymbol, sink);
    switch (c.type) {
      case CommandType::New:
        return on_new(c, sink);
      case CommandType::Cancel:
        return on_cancel(c, sink);
      case CommandType::Reduce:
        return on_reduce(c, sink);
      case CommandType::Replace:
        return on_replace(c, sink);
    }
  }

  std::size_t symbol_count() const noexcept { return books_.size(); }
  const Book& book(SymbolId symbol) const { return books_.at(symbol); }
  const EngineConfig& config() const noexcept { return config_; }
  const EngineStats& stats() const noexcept { return stats_; }
  Seq last_seq() const noexcept { return seq_; }

  TopOfBook top_of_book(SymbolId symbol) const {
    const Book& b = books_.at(symbol);
    return TopOfBook{b.best(Side::Buy), b.best(Side::Sell)};
  }

  // Up to max_levels aggregated levels of one side, best price first.
  std::vector<LevelView> depth(SymbolId symbol, Side side, std::size_t max_levels) const {
    std::vector<LevelView> out;
    books_.at(symbol).depth(side, max_levels, out);
    return out;
  }

 private:
  template <class Sink>
  struct TradeEmitter {
    Engine& engine;
    Sink& sink;
    SymbolId symbol;
    OrderId taker;
    Side side;

    void trade(OrderId maker, Price price, Qty qty, Qty maker_leaves, Qty taker_leaves) {
      Report r;
      r.seq = ++engine.seq_;
      r.order_id = taker;
      r.other_id = maker;
      r.price = price;
      r.qty = qty;
      r.leaves = taker_leaves;
      r.other_leaves = maker_leaves;
      r.symbol = symbol;
      r.type = ReportType::Trade;
      r.side = side;
      ++engine.stats_.trades;
      engine.stats_.traded_qty += qty;
      sink(std::as_const(r));
    }
  };

  template <class Sink>
  void reject(const Command& c, RejectReason reason, Sink& sink) {
    Report r;
    r.seq = ++seq_;
    r.order_id = c.order_id;
    r.other_id = c.new_order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.symbol = c.symbol;
    r.type = ReportType::Rejected;
    r.side = c.side;
    r.reason = reason;
    ++stats_.rejected;
    sink(std::as_const(r));
  }

  static RejectReason check_price_qty(const SymbolConfig& s, Price price, Qty qty) noexcept {
    if (price <= 0 || price > s.max_price) return RejectReason::InvalidPrice;
    if (price % s.tick_size != 0) return RejectReason::InvalidTick;
    if (qty <= 0 || qty > s.max_qty || qty % s.lot_size != 0) return RejectReason::InvalidQuantity;
    return RejectReason::None;
  }

  template <class Sink>
  void on_new(const Command& c, Sink& sink) {
    if (c.side != Side::Buy && c.side != Side::Sell) return reject(c, RejectReason::InvalidSide, sink);
    if (const RejectReason why = check_price_qty(config_.symbols[c.symbol], c.price, c.qty);
        why != RejectReason::None) {
      return reject(c, why, sink);
    }
    Book& book = books_[c.symbol];
    if (book.contains(c.order_id)) return reject(c, RejectReason::DuplicateOrderId, sink);
    // Checked up front, before any matching, so a rejected order never trades.
    if (book.size() >= config_.max_orders_per_book) return reject(c, RejectReason::BookFull, sink);

    Report r;
    r.seq = ++seq_;
    r.order_id = c.order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.leaves = c.qty;
    r.symbol = c.symbol;
    r.type = ReportType::Accepted;
    r.side = c.side;
    ++stats_.accepted;
    sink(std::as_const(r));

    TradeEmitter<Sink> emitter{*this, sink, c.symbol, c.order_id, c.side};
    book.add(c.order_id, c.side, c.price, c.qty, emitter);
  }

  template <class Sink>
  void on_cancel(const Command& c, Sink& sink) {
    Book& book = books_[c.symbol];
    const std::optional<OrderInfo> info = book.find(c.order_id);
    if (!info) return reject(c, RejectReason::UnknownOrderId, sink);
    Report r;
    r.qty = book.cancel(c.order_id);
    r.seq = ++seq_;
    r.order_id = c.order_id;
    r.price = info->price;
    r.symbol = c.symbol;
    r.type = ReportType::Canceled;
    r.side = info->side;
    ++stats_.canceled;
    sink(std::as_const(r));
  }

  template <class Sink>
  void on_reduce(const Command& c, Sink& sink) {
    Book& book = books_[c.symbol];
    const std::optional<OrderInfo> info = book.find(c.order_id);
    if (!info) return reject(c, RejectReason::UnknownOrderId, sink);
    if (c.qty <= 0 || c.qty >= info->leaves || c.qty % config_.symbols[c.symbol].lot_size != 0) {
      return reject(c, RejectReason::InvalidQuantity, sink);
    }
    Report r;
    r.qty = book.reduce(c.order_id, c.qty);
    r.seq = ++seq_;
    r.order_id = c.order_id;
    r.price = info->price;
    r.leaves = c.qty;
    r.symbol = c.symbol;
    r.type = ReportType::Reduced;
    r.side = info->side;
    ++stats_.reduced;
    sink(std::as_const(r));
  }

  template <class Sink>
  void on_replace(const Command& c, Sink& sink) {
    Book& book = books_[c.symbol];
    const std::optional<OrderInfo> info = book.find(c.order_id);
    if (!info) return reject(c, RejectReason::UnknownOrderId, sink);
    if (const RejectReason why = check_price_qty(config_.symbols[c.symbol], c.price, c.qty);
        why != RejectReason::None) {
      return reject(c, why, sink);
    }
    // Reusing the old id is allowed: the old order is gone before the new one enters.
    if (c.new_order_id != c.order_id && book.contains(c.new_order_id)) {
      return reject(c, RejectReason::DuplicateOrderId, sink);
    }

    Report r;
    r.other_leaves = book.cancel(c.order_id);
    r.seq = ++seq_;
    r.order_id = c.order_id;
    r.other_id = c.new_order_id;
    r.price = c.price;
    r.qty = c.qty;
    r.leaves = c.qty;
    r.symbol = c.symbol;
    r.type = ReportType::Replaced;
    r.side = info->side;
    ++stats_.replaced;
    sink(std::as_const(r));

    TradeEmitter<Sink> emitter{*this, sink, c.symbol, c.new_order_id, info->side};
    book.add(c.new_order_id, info->side, c.price, c.qty, emitter);
  }

  EngineConfig config_;
  std::vector<Book> books_;
  EngineStats stats_;
  Seq seq_ = 0;
};

}  // namespace me
