// Shared helpers for the test suite.
#pragma once

#include <gtest/gtest.h>

#include <ostream>
#include <vector>

#include "me/engine.hpp"
#include "me/map_book.hpp"
#include "me/pool_book.hpp"
#include "me/protocol.hpp"
#include "me/text_format.hpp"

namespace me {

// Lets gtest print readable diffs for reports and levels.
inline void PrintTo(const Report& r, std::ostream* os) {
  *os << format_report(r);
}
inline void PrintTo(const LevelView& l, std::ostream* os) {
  *os << l.price << " x " << l.qty << " (" << l.orders << ")";
}

namespace test {

using BookTypes = ::testing::Types<MapBook, PoolBook>;

inline Command new_order(SymbolId symbol, OrderId id, Side side, Price price, Qty qty) {
  Command c;
  c.type = CommandType::New;
  c.symbol = symbol;
  c.order_id = id;
  c.side = side;
  c.price = price;
  c.qty = qty;
  return c;
}
inline Command buy(OrderId id, Price price, Qty qty, SymbolId symbol = 0) {
  return new_order(symbol, id, Side::Buy, price, qty);
}
inline Command sell(OrderId id, Price price, Qty qty, SymbolId symbol = 0) {
  return new_order(symbol, id, Side::Sell, price, qty);
}
inline Command cancel(OrderId id, SymbolId symbol = 0) {
  Command c;
  c.type = CommandType::Cancel;
  c.symbol = symbol;
  c.order_id = id;
  return c;
}
inline Command reduce(OrderId id, Qty new_qty, SymbolId symbol = 0) {
  Command c;
  c.type = CommandType::Reduce;
  c.symbol = symbol;
  c.order_id = id;
  c.qty = new_qty;
  return c;
}
inline Command replace(OrderId id, OrderId new_id, Price price, Qty qty, SymbolId symbol = 0) {
  Command c;
  c.type = CommandType::Replace;
  c.symbol = symbol;
  c.order_id = id;
  c.new_order_id = new_id;
  c.price = price;
  c.qty = qty;
  return c;
}

inline EngineConfig make_config(std::size_t symbols = 1, Price tick = 1, Qty lot = 1, std::uint32_t max_orders = 1024) {
  EngineConfig config;
  SymbolConfig s;
  s.tick_size = tick;
  s.lot_size = lot;
  config.symbols.assign(symbols, s);
  config.max_orders_per_book = max_orders;
  return config;
}

// Runs one command and returns its reports.
template <class Book>
std::vector<Report> run(Engine<Book>& engine, const Command& c) {
  std::vector<Report> out;
  engine.process(c, [&out](const Report& r) { out.push_back(r); });
  return out;
}

// Runs a command the test expects to be rejected and returns the reason.
template <class Book>
RejectReason reject_reason(Engine<Book>& engine, const Command& c) {
  const std::vector<Report> out = run(engine, c);
  if (out.size() != 1 || out[0].type != ReportType::Rejected) return RejectReason::None;
  return out[0].reason;
}

struct TradeSummary {
  OrderId maker;
  Price price;
  Qty qty;
  friend bool operator==(const TradeSummary&, const TradeSummary&) = default;
};
inline void PrintTo(const TradeSummary& t, std::ostream* os) {
  *os << "maker=" << t.maker << " px=" << t.price << " qty=" << t.qty;
}

inline std::vector<TradeSummary> trades(const std::vector<Report>& reports) {
  std::vector<TradeSummary> out;
  for (const Report& r : reports) {
    if (r.type == ReportType::Trade) out.push_back(TradeSummary{r.other_id, r.price, r.qty});
  }
  return out;
}

}  // namespace test
}  // namespace me
