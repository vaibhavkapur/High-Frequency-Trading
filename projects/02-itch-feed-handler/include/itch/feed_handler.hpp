// Decoder + book builder + trade statistics, glued into one per-message call.
//
// Which messages touch the displayed book:
//   A, F  add          E, C  execute (reduce)     X  cancel (reduce)
//   D     delete       U     replace
// Which do not:
//   P  trade against a non-displayed order - reported as a trade only.
//   Q  cross trade - reported as a trade only; the orders that took part are
//      removed by their own E/C/D messages.
//   B  broken trade - counted; already-published statistics are not unwound.
//
// Volume accounting avoids double counting: an Order Executed With Price
// message marked non-printable ('N') updates the book but is left out of
// volume, because the same shares are reported by a cross (Q) print.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "itch/book.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"
#include "itch/order_index.hpp"

namespace itch {

struct TradeEvent {
  Timestamp timestamp = 0;
  std::uint16_t locate = 0;
  char kind = 0;  // message type that reported it: 'E', 'C', 'P' or 'Q'
  Price4 price = 0;
  std::uint64_t qty = 0;
  std::uint64_t match_number = 0;
  // Side of the incoming (liquidity-taking) order: 'B', 'S', or ' ' when the
  // feed does not reveal it (P and Q).
  char aggressor = ' ';
  bool printable = true;
};

struct SymbolStats {
  std::uint64_t trades = 0;         // printable E/C plus P
  std::uint64_t volume = 0;         // shares in those trades
  std::uint64_t buy_volume = 0;     // buyer-initiated displayed executions
  std::uint64_t sell_volume = 0;    // seller-initiated displayed executions
  std::uint64_t hidden_volume = 0;  // P: executions against non-displayed orders
  std::uint64_t cross_volume = 0;   // Q: opening/closing/halt cross prints
};

struct FeedCounters {
  std::uint64_t messages = 0;
  std::uint64_t decoded = 0;
  std::uint64_t empty = 0;
  std::uint64_t unknown_type = 0;
  std::uint64_t truncated = 0;
  std::uint64_t oversized = 0;
  std::uint64_t invalid_field = 0;
  std::uint64_t broken_trades = 0;
  std::uint64_t locate_mismatch = 0;              // header locate disagreed with the order's locate
  std::array<std::uint64_t, 256> seen_by_type{};  // by first byte, decoded or not
  std::array<std::uint64_t, 256> decoded_by_type{};  // successfully decoded
};

struct ProcessResult {
  DecodeStatus status = DecodeStatus::Ok;
  Timestamp timestamp = 0;
  std::int32_t book_locate = -1;  // stock locate whose displayed book changed, or -1
  bool has_trade = false;
  TradeEvent trade;
};

template <class Index = FlatOrderIndex>
class FeedHandler {
 public:
  // Processes one unframed message payload. Never throws on malformed input;
  // the outcome is in the returned status and the counters.
  ProcessResult process(std::span<const std::uint8_t> payload) {
    ProcessResult r;
    ++counters_.messages;
    if (!payload.empty()) ++counters_.seen_by_type[payload[0]];
    r.status = decode(payload, [this, &r](const auto& m) {
      r.timestamp = m.hdr.timestamp;
      this->on(m, r);
    });
    switch (r.status) {
      case DecodeStatus::Ok:
        ++counters_.decoded;
        ++counters_.decoded_by_type[payload[0]];
        break;
      case DecodeStatus::Empty:
        ++counters_.empty;
        break;
      case DecodeStatus::UnknownType:
        ++counters_.unknown_type;
        break;
      case DecodeStatus::Truncated:
        ++counters_.truncated;
        break;
      case DecodeStatus::Oversized:
        ++counters_.oversized;
        break;
      case DecodeStatus::InvalidField:
        ++counters_.invalid_field;
        break;
    }
    return r;
  }

  [[nodiscard]] const BookBuilder<Index>& books() const noexcept { return book_; }
  [[nodiscard]] const FeedCounters& counters() const noexcept { return counters_; }
  [[nodiscard]] const SymbolStats* stats(std::uint16_t locate) const noexcept {
    return locate < stats_.size() ? &stats_[locate] : nullptr;
  }

 private:
  // Messages that carry no book or trade information.
  template <class T>
  void on(const T&, ProcessResult&) noexcept {}

  void on(const StockDirectory& m, ProcessResult&) {
    book_.set_symbol(m.hdr.stock_locate, m.stock);
  }

  void on(const AddOrder& m, ProcessResult& r) {
    add(m.hdr, m.order_ref, m.side, m.shares, m.stock, m.price, r);
  }

  void on(const AddOrderMpid& m, ProcessResult& r) {
    add(m.hdr, m.order_ref, m.side, m.shares, m.stock, m.price, r);
  }

  void on(const OrderExecuted& m, ProcessResult& r) {
    const BookResult br = book_.reduce(m.order_ref, m.executed_shares);
    if (!applied(m.hdr, br, r)) return;
    // An execution at the resting order's own price, always printable.
    trade(r, m.hdr.timestamp, br, 'E', br.price, m.match_number, true);
  }

  void on(const OrderExecutedWithPrice& m, ProcessResult& r) {
    const BookResult br = book_.reduce(m.order_ref, m.executed_shares);
    if (!applied(m.hdr, br, r)) return;
    trade(r, m.hdr.timestamp, br, 'C', m.execution_price, m.match_number, m.printable != 'N');
  }

  void on(const OrderCancel& m, ProcessResult& r) {
    applied(m.hdr, book_.reduce(m.order_ref, m.cancelled_shares), r);
  }

  void on(const OrderDelete& m, ProcessResult& r) { applied(m.hdr, book_.remove(m.order_ref), r); }

  void on(const OrderReplace& m, ProcessResult& r) {
    const BookResult br = book_.replace(m.original_order_ref, m.new_order_ref, m.shares, m.price);
    // A duplicate new reference still removed the original, so the book moved.
    if (br.status == BookStatus::UnknownOrder) return;
    check_locate(m.hdr, br.locate);
    r.book_locate = br.locate;
  }

  void on(const Trade& m, ProcessResult& r) {
    r.has_trade = true;
    r.trade = {m.hdr.timestamp, m.hdr.stock_locate, 'P', m.price,
               m.shares,        m.match_number,     ' ', true};
    SymbolStats& s = stats_mut(m.hdr.stock_locate);
    ++s.trades;
    s.volume += m.shares;
    s.hidden_volume += m.shares;
  }

  void on(const CrossTrade& m, ProcessResult& r) {
    r.has_trade = true;
    r.trade = {m.hdr.timestamp, m.hdr.stock_locate, 'Q', m.cross_price,
               m.shares,        m.match_number,     ' ', true};
    stats_mut(m.hdr.stock_locate).cross_volume += m.shares;
  }

  void on(const BrokenTrade&, ProcessResult&) noexcept { ++counters_.broken_trades; }

  void add(const Header& hdr, OrderRef ref, Side side, std::uint32_t shares, const Symbol& stock,
           Price4 price, ProcessResult& r) {
    const BookResult br = book_.add(ref, hdr.stock_locate, side, shares, price);
    if (br.status != BookStatus::Ok) return;
    r.book_locate = hdr.stock_locate;
    // Learn the symbol from order flow when no Stock Directory was seen
    // (e.g. a replay that starts mid-session).
    if (!book_.book(hdr.stock_locate)->has_symbol) book_.set_symbol(hdr.stock_locate, stock);
  }

  // Common handling for operations on an existing order. Returns false if the
  // order was unknown and nothing happened.
  bool applied(const Header& hdr, const BookResult& br, ProcessResult& r) noexcept {
    if (br.status == BookStatus::UnknownOrder) return false;
    check_locate(hdr, br.locate);
    r.book_locate = br.locate;
    return true;
  }

  void check_locate(const Header& hdr, std::uint16_t order_locate) noexcept {
    if (hdr.stock_locate != order_locate) ++counters_.locate_mismatch;
  }

  void trade(ProcessResult& r, Timestamp ts, const BookResult& br, char kind, Price4 price,
             std::uint64_t match, bool printable) {
    // The resting order was hit, so the aggressor is on the opposite side.
    const char aggressor = br.side == Side::Buy ? 'S' : 'B';
    r.has_trade = true;
    r.trade = {ts, br.locate, kind, price, br.qty, match, aggressor, printable};
    if (!printable) return;
    SymbolStats& s = stats_mut(br.locate);
    ++s.trades;
    s.volume += br.qty;
    (aggressor == 'B' ? s.buy_volume : s.sell_volume) += br.qty;
  }

  SymbolStats& stats_mut(std::uint16_t locate) {
    if (locate >= stats_.size()) stats_.resize(std::size_t{locate} + 1);
    return stats_[locate];
  }

  BookBuilder<Index> book_;
  std::vector<SymbolStats> stats_;
  FeedCounters counters_;
};

}  // namespace itch
