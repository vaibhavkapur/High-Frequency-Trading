// Binary wire protocol: fixed-size little-endian command and report records.
//
// A command stream is a plain concatenation of 40-byte Command records; a report
// stream is a concatenation of 64-byte Report records. There is no framing or
// header. Every byte of both records is defined (padding is explicit and always
// written as zero), which is what makes replay output byte-identical.
// See docs/PROTOCOL.md for the field-by-field layout.
#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

#include "me/types.hpp"

namespace me {

enum class CommandType : std::uint8_t {
  New = 1,      // enter a limit order
  Cancel = 2,   // remove a resting order
  Reduce = 3,   // lower the remaining quantity of a resting order, keeping its priority
  Replace = 4,  // atomically cancel a resting order and enter a new one (loses priority)
};

struct Command {
  CommandType type{};
  Side side{};  // New only
  std::uint16_t reserved = 0;
  SymbolId symbol = 0;
  OrderId order_id = 0;      // New: id of the new order. Others: id of the resting order
  OrderId new_order_id = 0;  // Replace only: id of the replacement order
  Price price = 0;           // New / Replace
  Qty qty = 0;               // New / Replace: order quantity. Reduce: new remaining quantity

  friend bool operator==(const Command&, const Command&) = default;
};

enum class ReportType : std::uint8_t {
  Accepted = 1,  // a New order passed validation (trades, if any, follow)
  Rejected = 2,  // a command failed validation; book state is unchanged
  Trade = 3,     // one maker/taker fill
  Canceled = 4,  // a resting order was removed by Cancel
  Reduced = 5,   // a resting order's remaining quantity was lowered
  Replaced = 6,  // a resting order was removed and its replacement accepted (trades may follow)
};

enum class RejectReason : std::uint8_t {
  None = 0,
  UnknownCommand = 1,
  UnknownSymbol = 2,
  InvalidSide = 3,
  InvalidPrice = 4,     // price <= 0 or above the symbol's maximum
  InvalidTick = 5,      // price is not a multiple of the symbol's tick size
  InvalidQuantity = 6,  // qty <= 0, above the maximum, not a lot multiple, or a non-decreasing Reduce
  DuplicateOrderId = 7,
  UnknownOrderId = 8,
  BookFull = 9,  // the symbol already holds max_orders_per_book resting orders
};

// Field meaning by report type:
//
//   type      order_id   other_id   side        price        qty            leaves          other_leaves
//   Accepted  new order  0          order side  limit price  order qty      order qty       0
//   Rejected  cmd id     cmd new id cmd side    cmd price    cmd qty        0               0
//   Trade     taker      maker      taker side  maker price  traded qty     taker remaining maker remaining
//   Canceled  order      0          order side  order price  qty removed    0               0
//   Reduced   order      0          order side  order price  qty removed    new remaining   0
//   Replaced  old order  new order  order side  new price    new order qty  new order qty   old qty removed
struct Report {
  Seq seq = 0;
  OrderId order_id = 0;
  OrderId other_id = 0;
  Price price = 0;
  Qty qty = 0;
  Qty leaves = 0;
  Qty other_leaves = 0;
  SymbolId symbol = 0;
  ReportType type{};
  Side side{};
  RejectReason reason = RejectReason::None;
  std::uint8_t reserved = 0;

  friend bool operator==(const Report&, const Report&) = default;
};

inline constexpr std::size_t kCommandSize = 40;
inline constexpr std::size_t kReportSize = 64;

static_assert(sizeof(Command) == kCommandSize);
static_assert(sizeof(Report) == kReportSize);
static_assert(std::is_trivially_copyable_v<Command> && std::is_standard_layout_v<Command>);
static_assert(std::is_trivially_copyable_v<Report> && std::is_standard_layout_v<Report>);
static_assert(std::has_unique_object_representations_v<Command>, "Command must have no implicit padding");
static_assert(std::has_unique_object_representations_v<Report>, "Report must have no implicit padding");

// Decoding never fails: any 40 bytes form a Command. Out-of-range enum values and
// nonsensical fields are caught by the engine's validator, which rejects them.
inline Command decode_command(std::span<const std::byte, kCommandSize> bytes) noexcept {
  Command c;
  std::memcpy(&c, bytes.data(), kCommandSize);
  return c;
}

inline void encode_command(const Command& c, std::span<std::byte, kCommandSize> out) noexcept {
  std::memcpy(out.data(), &c, kCommandSize);
}

inline Report decode_report(std::span<const std::byte, kReportSize> bytes) noexcept {
  Report r;
  std::memcpy(&r, bytes.data(), kReportSize);
  return r;
}

inline void encode_report(const Report& r, std::span<std::byte, kReportSize> out) noexcept {
  std::memcpy(out.data(), &r, kReportSize);
}

}  // namespace me
