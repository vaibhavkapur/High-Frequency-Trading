// Typed event model for Nasdaq TotalView-ITCH 5.0.
//
// Field names, order and widths follow the "Nasdaq TotalView-ITCH 5.0"
// specification (see docs/protocol.md for the citation and message matrix).
// These structs are the *decoded* representation: they are never overlaid on
// wire bytes. kLength is the wire size of the message including the 1-byte
// message type, excluding any transport framing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace itch {

using Timestamp = std::uint64_t;  // nanoseconds since midnight (48 bits on the wire)
using Price4 = std::uint32_t;     // fixed point, 4 implied decimals
using Price8 = std::uint64_t;     // fixed point, 8 implied decimals (MWCB levels)
using OrderRef = std::uint64_t;

enum class Side : char { Buy = 'B', Sell = 'S' };

// 8-character, left-justified, space-padded security symbol.
struct Symbol {
  std::array<char, 8> raw{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    std::size_t n = raw.size();
    while (n > 0 && raw[n - 1] == ' ') --n;
    return {raw.data(), n};
  }

  [[nodiscard]] static constexpr Symbol from(std::string_view s) noexcept {
    Symbol out;
    for (std::size_t i = 0; i < out.raw.size() && i < s.size(); ++i) out.raw[i] = s[i];
    return out;
  }

  friend constexpr bool operator==(const Symbol&, const Symbol&) = default;
};

// Fields common to every ITCH 5.0 message, following the message type byte.
struct Header {
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  Timestamp timestamp{};

  friend constexpr bool operator==(const Header&, const Header&) = default;
};

// ---- System / administrative messages --------------------------------------

struct SystemEvent {
  static constexpr char kType = 'S';
  static constexpr std::size_t kLength = 12;
  Header hdr;
  char event_code{};
  friend constexpr bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

struct StockDirectory {
  static constexpr char kType = 'R';
  static constexpr std::size_t kLength = 39;
  Header hdr;
  Symbol stock;
  char market_category{};
  char financial_status{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  std::array<char, 2> issue_subtype{};
  char authenticity{};
  char short_sale_threshold{};
  char ipo_flag{};
  char luld_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage{};
  char inverse_indicator{};
  friend constexpr bool operator==(const StockDirectory&, const StockDirectory&) = default;
};

struct StockTradingAction {
  static constexpr char kType = 'H';
  static constexpr std::size_t kLength = 25;
  Header hdr;
  Symbol stock;
  char trading_state{};
  char reserved{};
  std::array<char, 4> reason{};
  friend constexpr bool operator==(const StockTradingAction&, const StockTradingAction&) = default;
};

struct RegShoRestriction {
  static constexpr char kType = 'Y';
  static constexpr std::size_t kLength = 20;
  Header hdr;
  Symbol stock;
  char action{};
  friend constexpr bool operator==(const RegShoRestriction&, const RegShoRestriction&) = default;
};

struct MarketParticipantPosition {
  static constexpr char kType = 'L';
  static constexpr std::size_t kLength = 26;
  Header hdr;
  std::array<char, 4> mpid{};
  Symbol stock;
  char primary_market_maker{};
  char market_maker_mode{};
  char participant_state{};
  friend constexpr bool operator==(const MarketParticipantPosition&,
                                   const MarketParticipantPosition&) = default;
};

struct MwcbDeclineLevel {
  static constexpr char kType = 'V';
  static constexpr std::size_t kLength = 35;
  Header hdr;
  Price8 level1{};
  Price8 level2{};
  Price8 level3{};
  friend constexpr bool operator==(const MwcbDeclineLevel&, const MwcbDeclineLevel&) = default;
};

struct MwcbStatus {
  static constexpr char kType = 'W';
  static constexpr std::size_t kLength = 12;
  Header hdr;
  char breached_level{};
  friend constexpr bool operator==(const MwcbStatus&, const MwcbStatus&) = default;
};

struct IpoQuotingPeriod {
  static constexpr char kType = 'K';
  static constexpr std::size_t kLength = 28;
  Header hdr;
  Symbol stock;
  std::uint32_t release_time{};  // seconds since midnight
  char release_qualifier{};
  Price4 ipo_price{};
  friend constexpr bool operator==(const IpoQuotingPeriod&, const IpoQuotingPeriod&) = default;
};

struct LuldAuctionCollar {
  static constexpr char kType = 'J';
  static constexpr std::size_t kLength = 35;
  Header hdr;
  Symbol stock;
  Price4 reference_price{};
  Price4 upper_price{};
  Price4 lower_price{};
  std::uint32_t extension{};
  friend constexpr bool operator==(const LuldAuctionCollar&, const LuldAuctionCollar&) = default;
};

struct OperationalHalt {
  static constexpr char kType = 'h';
  static constexpr std::size_t kLength = 21;
  Header hdr;
  Symbol stock;
  char market_code{};
  char action{};
  friend constexpr bool operator==(const OperationalHalt&, const OperationalHalt&) = default;
};

// ---- Order flow -------------------------------------------------------------

struct AddOrder {
  static constexpr char kType = 'A';
  static constexpr std::size_t kLength = 36;
  Header hdr;
  OrderRef order_ref{};
  Side side{Side::Buy};
  std::uint32_t shares{};
  Symbol stock;
  Price4 price{};
  friend constexpr bool operator==(const AddOrder&, const AddOrder&) = default;
};

struct AddOrderMpid {
  static constexpr char kType = 'F';
  static constexpr std::size_t kLength = 40;
  Header hdr;
  OrderRef order_ref{};
  Side side{Side::Buy};
  std::uint32_t shares{};
  Symbol stock;
  Price4 price{};
  std::array<char, 4> attribution{};
  friend constexpr bool operator==(const AddOrderMpid&, const AddOrderMpid&) = default;
};

struct OrderExecuted {
  static constexpr char kType = 'E';
  static constexpr std::size_t kLength = 31;
  Header hdr;
  OrderRef order_ref{};
  std::uint32_t executed_shares{};
  std::uint64_t match_number{};
  friend constexpr bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

struct OrderExecutedWithPrice {
  static constexpr char kType = 'C';
  static constexpr std::size_t kLength = 36;
  Header hdr;
  OrderRef order_ref{};
  std::uint32_t executed_shares{};
  std::uint64_t match_number{};
  char printable{};  // 'Y' printable, 'N' non-printable (already in a cross/bulk print)
  Price4 execution_price{};
  friend constexpr bool operator==(const OrderExecutedWithPrice&,
                                   const OrderExecutedWithPrice&) = default;
};

struct OrderCancel {
  static constexpr char kType = 'X';
  static constexpr std::size_t kLength = 23;
  Header hdr;
  OrderRef order_ref{};
  std::uint32_t cancelled_shares{};
  friend constexpr bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

struct OrderDelete {
  static constexpr char kType = 'D';
  static constexpr std::size_t kLength = 19;
  Header hdr;
  OrderRef order_ref{};
  friend constexpr bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

struct OrderReplace {
  static constexpr char kType = 'U';
  static constexpr std::size_t kLength = 35;
  Header hdr;
  OrderRef original_order_ref{};
  OrderRef new_order_ref{};
  std::uint32_t shares{};
  Price4 price{};
  friend constexpr bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

// ---- Trades -----------------------------------------------------------------

// Execution against a non-displayed order. Never references the visible book.
struct Trade {
  static constexpr char kType = 'P';
  static constexpr std::size_t kLength = 44;
  Header hdr;
  OrderRef order_ref{};
  Side side{Side::Buy};
  std::uint32_t shares{};
  Symbol stock;
  Price4 price{};
  std::uint64_t match_number{};
  friend constexpr bool operator==(const Trade&, const Trade&) = default;
};

struct CrossTrade {
  static constexpr char kType = 'Q';
  static constexpr std::size_t kLength = 40;
  Header hdr;
  std::uint64_t shares{};
  Symbol stock;
  Price4 cross_price{};
  std::uint64_t match_number{};
  char cross_type{};
  friend constexpr bool operator==(const CrossTrade&, const CrossTrade&) = default;
};

struct BrokenTrade {
  static constexpr char kType = 'B';
  static constexpr std::size_t kLength = 19;
  Header hdr;
  std::uint64_t match_number{};
  friend constexpr bool operator==(const BrokenTrade&, const BrokenTrade&) = default;
};

// ---- Auction / retail information ------------------------------------------

struct Noii {
  static constexpr char kType = 'I';
  static constexpr std::size_t kLength = 50;
  Header hdr;
  std::uint64_t paired_shares{};
  std::uint64_t imbalance_shares{};
  char imbalance_direction{};
  Symbol stock;
  Price4 far_price{};
  Price4 near_price{};
  Price4 current_reference_price{};
  char cross_type{};
  char price_variation_indicator{};
  friend constexpr bool operator==(const Noii&, const Noii&) = default;
};

struct Rpii {
  static constexpr char kType = 'N';
  static constexpr std::size_t kLength = 20;
  Header hdr;
  Symbol stock;
  char interest_flag{};
  friend constexpr bool operator==(const Rpii&, const Rpii&) = default;
};

// Direct Listing with Capital Raise price discovery.
struct DlcrPriceDiscovery {
  static constexpr char kType = 'O';
  static constexpr std::size_t kLength = 48;
  Header hdr;
  Symbol stock;
  char open_eligibility_status{};
  Price4 min_allowable_price{};
  Price4 max_allowable_price{};
  Price4 near_execution_price{};
  std::uint64_t near_execution_time{};
  Price4 lower_price_range_collar{};
  Price4 upper_price_range_collar{};
  friend constexpr bool operator==(const DlcrPriceDiscovery&, const DlcrPriceDiscovery&) = default;
};

// X-macro over every supported message type. This is the single source of
// truth for the decoder switch, the variant, and the compile-time size checks.
#define ITCH_FOR_EACH_MESSAGE(X) \
  X(SystemEvent)                 \
  X(StockDirectory)              \
  X(StockTradingAction)          \
  X(RegShoRestriction)           \
  X(MarketParticipantPosition)   \
  X(MwcbDeclineLevel)            \
  X(MwcbStatus)                  \
  X(IpoQuotingPeriod)            \
  X(LuldAuctionCollar)           \
  X(OperationalHalt)             \
  X(AddOrder)                    \
  X(AddOrderMpid)                \
  X(OrderExecuted)               \
  X(OrderExecutedWithPrice)      \
  X(OrderCancel)                 \
  X(OrderDelete)                 \
  X(OrderReplace)                \
  X(Trade)                       \
  X(CrossTrade)                  \
  X(BrokenTrade)                 \
  X(Noii)                        \
  X(Rpii)                        \
  X(DlcrPriceDiscovery)

// Wire length for a message type byte, or 0 if the type is not supported.
[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
  switch (type) {
#define ITCH_LEN_CASE(T) \
  case T::kType:         \
    return T::kLength;
    ITCH_FOR_EACH_MESSAGE(ITCH_LEN_CASE)
#undef ITCH_LEN_CASE
    default:
      return 0;
  }
}

inline constexpr std::size_t kMaxMessageLength = 50;

}  // namespace itch
