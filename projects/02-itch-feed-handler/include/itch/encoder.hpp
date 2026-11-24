// ITCH 5.0 message encoder.
//
// Used to author test fixtures and synthetic replay files. It is the exact
// inverse of decoder.hpp: encode(decode(bytes)) == bytes for every valid
// payload (tests/test_decoder.cpp checks this for all message types).
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "itch/decoder.hpp"
#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

namespace detail {

class Writer {
 public:
  explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t v) { out_.push_back(v); }
  void ch(char v) { out_.push_back(static_cast<std::uint8_t>(v)); }
  void u16(std::uint16_t v) { be(v, 2); }
  void u32(std::uint32_t v) { be(v, 4); }
  void u48(std::uint64_t v) { be(v, 6); }
  void u64(std::uint64_t v) { be(v, 8); }
  template <std::size_t N>
  void chars(const std::array<char, N>& a) {
    for (char c : a) ch(c);
  }

 private:
  void be(std::uint64_t v, std::size_t bytes) {
    const std::size_t at = out_.size();
    out_.resize(at + bytes);
    store_be(out_.data() + at, v, bytes);
  }

  std::vector<std::uint8_t>& out_;
};

inline void write_header(Writer& w, char type, const Header& h) {
  w.ch(type);
  w.u16(h.stock_locate);
  w.u16(h.tracking_number);
  w.u48(h.timestamp);
}

inline void write(Writer& w, const SystemEvent& m) {
  write_header(w, m.kType, m.hdr);
  w.ch(m.event_code);
}

inline void write(Writer& w, const StockDirectory& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.market_category);
  w.ch(m.financial_status);
  w.u32(m.round_lot_size);
  w.ch(m.round_lots_only);
  w.ch(m.issue_classification);
  w.chars(m.issue_subtype);
  w.ch(m.authenticity);
  w.ch(m.short_sale_threshold);
  w.ch(m.ipo_flag);
  w.ch(m.luld_tier);
  w.ch(m.etp_flag);
  w.u32(m.etp_leverage);
  w.ch(m.inverse_indicator);
}

inline void write(Writer& w, const StockTradingAction& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.trading_state);
  w.ch(m.reserved);
  w.chars(m.reason);
}

inline void write(Writer& w, const RegShoRestriction& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.action);
}

inline void write(Writer& w, const MarketParticipantPosition& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.mpid);
  w.chars(m.stock.raw);
  w.ch(m.primary_market_maker);
  w.ch(m.market_maker_mode);
  w.ch(m.participant_state);
}

inline void write(Writer& w, const MwcbDeclineLevel& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.level1);
  w.u64(m.level2);
  w.u64(m.level3);
}

inline void write(Writer& w, const MwcbStatus& m) {
  write_header(w, m.kType, m.hdr);
  w.ch(m.breached_level);
}

inline void write(Writer& w, const IpoQuotingPeriod& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.u32(m.release_time);
  w.ch(m.release_qualifier);
  w.u32(m.ipo_price);
}

inline void write(Writer& w, const LuldAuctionCollar& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.u32(m.reference_price);
  w.u32(m.upper_price);
  w.u32(m.lower_price);
  w.u32(m.extension);
}

inline void write(Writer& w, const OperationalHalt& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.market_code);
  w.ch(m.action);
}

inline void write(Writer& w, const AddOrder& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.ch(static_cast<char>(m.side));
  w.u32(m.shares);
  w.chars(m.stock.raw);
  w.u32(m.price);
}

inline void write(Writer& w, const AddOrderMpid& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.ch(static_cast<char>(m.side));
  w.u32(m.shares);
  w.chars(m.stock.raw);
  w.u32(m.price);
  w.chars(m.attribution);
}

inline void write(Writer& w, const OrderExecuted& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.u32(m.executed_shares);
  w.u64(m.match_number);
}

inline void write(Writer& w, const OrderExecutedWithPrice& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.u32(m.executed_shares);
  w.u64(m.match_number);
  w.ch(m.printable);
  w.u32(m.execution_price);
}

inline void write(Writer& w, const OrderCancel& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.u32(m.cancelled_shares);
}

inline void write(Writer& w, const OrderDelete& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
}

inline void write(Writer& w, const OrderReplace& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.original_order_ref);
  w.u64(m.new_order_ref);
  w.u32(m.shares);
  w.u32(m.price);
}

inline void write(Writer& w, const Trade& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.order_ref);
  w.ch(static_cast<char>(m.side));
  w.u32(m.shares);
  w.chars(m.stock.raw);
  w.u32(m.price);
  w.u64(m.match_number);
}

inline void write(Writer& w, const CrossTrade& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.shares);
  w.chars(m.stock.raw);
  w.u32(m.cross_price);
  w.u64(m.match_number);
  w.ch(m.cross_type);
}

inline void write(Writer& w, const BrokenTrade& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.match_number);
}

inline void write(Writer& w, const Noii& m) {
  write_header(w, m.kType, m.hdr);
  w.u64(m.paired_shares);
  w.u64(m.imbalance_shares);
  w.ch(m.imbalance_direction);
  w.chars(m.stock.raw);
  w.u32(m.far_price);
  w.u32(m.near_price);
  w.u32(m.current_reference_price);
  w.ch(m.cross_type);
  w.ch(m.price_variation_indicator);
}

inline void write(Writer& w, const Rpii& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.interest_flag);
}

inline void write(Writer& w, const DlcrPriceDiscovery& m) {
  write_header(w, m.kType, m.hdr);
  w.chars(m.stock.raw);
  w.ch(m.open_eligibility_status);
  w.u32(m.min_allowable_price);
  w.u32(m.max_allowable_price);
  w.u32(m.near_execution_price);
  w.u64(m.near_execution_time);
  w.u32(m.lower_price_range_collar);
  w.u32(m.upper_price_range_collar);
}

}  // namespace detail

// Appends the wire form of `m` (type byte first, no framing) to `out`.
template <class T>
inline void encode(const T& m, std::vector<std::uint8_t>& out) {
  [[maybe_unused]] const std::size_t before = out.size();
  detail::Writer w{out};
  detail::write(w, m);
  assert(out.size() - before == T::kLength);
}

inline void encode(const Message& m, std::vector<std::uint8_t>& out) {
  std::visit(
      [&out](const auto& v) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) encode(v, out);
      },
      m);
}

// Appends `m` preceded by the 2-byte big-endian length used by Nasdaq's
// historical ITCH files (and by MoldUDP64 message blocks).
template <class T>
inline void encode_framed(const T& m, std::vector<std::uint8_t>& out) {
  const std::size_t at = out.size();
  out.resize(at + 2);
  store_be(out.data() + at, T::kLength, 2);
  encode(m, out);
}

}  // namespace itch
