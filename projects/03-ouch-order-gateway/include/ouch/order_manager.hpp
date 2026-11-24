// Per-order finite-state machine and exposure accounting.
//
// OrderManager performs no I/O. Its state is a pure function of the ordered
// stream of events applied to it (requests sent, exchange messages received,
// disconnects, resends), which is exactly what the append-only journal
// records. Replaying a journal therefore rebuilds identical state.
//
// The exchange is the source of truth. Every exchange message is applied
// idempotently: a duplicated or replayed message never changes quantities.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ouch/types.hpp"
#include "ouch/wire.hpp"

namespace ouch {

enum class OrderState : std::uint8_t {
  PendingNew,       // sent, no response yet
  Reconciling,      // sent, then the session dropped before any response: outcome unknown
  Live,             // accepted, nothing executed
  PartiallyFilled,  // accepted, some shares executed, some still open
  Filled,           // terminal
  Canceled,         // terminal (may have partial executions)
  Rejected,         // terminal
  Replaced,         // terminal: superseded by a replacement order
};

enum class PendingRequest : std::uint8_t { None, Cancel, Replace };

const char* to_string(OrderState s);
constexpr bool is_terminal(OrderState s) {
  return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected ||
         s == OrderState::Replaced;
}
constexpr bool is_unacked(OrderState s) {
  return s == OrderState::PendingNew || s == OrderState::Reconciling;
}

// Reject reason recorded when the OMS itself terminates a replacement order
// because the order it was replacing reached a terminal state first.
inline constexpr char kReasonLocalAbort = '#';

struct Order {
  std::uint32_t idx = 0;        // creation index; stable for the life of the OrderManager
  wire::EnterOrder req;         // static attributes; req.price tracks the accepted price
  std::uint32_t qty = 0;        // current order size: always cum_qty + leaves while working
  std::uint32_t cum_qty = 0;    // executed shares
  std::uint32_t leaves = 0;     // open shares counted as exposure
  std::uint64_t notional = 0;   // sum(exec shares * exec price)
  std::uint64_t order_ref = 0;  // exchange order reference number (0 until known)
  OrderState state = OrderState::PendingNew;
  PendingRequest pending = PendingRequest::None;
  bool pending_uncertain = false;    // pending request may not have reached the exchange
  bool locally_aborted = false;      // see kReasonLocalAbort
  std::uint32_t cancel_to = 0;       // target open quantity of the pending cancel
  std::uint32_t cancel_credits = 0;  // cancel requests sent and not yet answered by a Canceled
  std::uint32_t replace_shares = 0;  // (replacement orders) shares field of the request
  std::uint32_t est_open = 0;  // (replacement orders) open size assumed until Replaced arrives
  Token replaces;              // (replacement orders) token being replaced
  Token pending_token;         // replacement token of the pending replace
  Token replaced_by;
  char reason = 0;  // reject / cancel reason code
  std::uint32_t resends = 0;
  std::uint64_t sent_ns = 0;
  std::uint64_t acked_ns = 0;

  [[nodiscard]] const Token& token() const { return req.token; }
  [[nodiscard]] bool working() const {
    return state == OrderState::Live || state == OrderState::PartiallyFilled;
  }
};

enum class OrderEvent : std::uint8_t {
  Submitted,
  CancelRequested,
  ReplaceRequested,
  Accepted,
  Rejected,
  Executed,
  Canceled,  // full or partial reduction
  Replaced,  // reported on the replacement order
  Uncertain,
  Resent,
};

const char* to_string(OrderEvent e);

struct OrderUpdate {
  OrderEvent event;
  const Order* order;
  std::uint32_t shares = 0;  // executed or canceled shares for this event
  std::uint32_t price = 0;   // execution price
};

struct SymbolExposure {
  std::int64_t position = 0;    // net executed shares (buys - sells)
  std::uint64_t open_buy = 0;   // open + unacknowledged buy shares
  std::uint64_t open_sell = 0;  // open + unacknowledged sell shares
  friend bool operator==(const SymbolExposure&, const SymbolExposure&) = default;
};

class OrderManager {
 public:
  using Listener = std::function<void(const OrderUpdate&)>;

  struct Stats {
    std::uint64_t duplicates = 0;        // exchange messages dropped as already applied
    std::uint64_t unknown_tokens = 0;    // exchange messages for tokens we never sent
    std::uint64_t implicit_accepts = 0;  // execution/cancel seen before the Accepted
    std::uint64_t anomalies = 0;         // overfills, fills on dead orders, etc.
    std::uint64_t uncertain = 0;         // requests left in doubt by a disconnect
    std::uint64_t revived = 0;           // locally aborted replacements the exchange confirmed
    friend bool operator==(const Stats&, const Stats&) = default;
  };

  void set_listener(Listener l) { listener_ = std::move(l); }

  // --- requests we put on the wire. Return false (and change nothing) if the
  // request is not valid for the order's current state.
  bool on_enter_sent(const wire::EnterOrder& m, std::uint64_t ts);
  bool on_cancel_sent(const wire::CancelOrder& m, std::uint64_t ts);
  bool on_replace_sent(const wire::ReplaceOrder& m, std::uint64_t ts);
  bool on_request_sent(const wire::InboundMessage& m, std::uint64_t ts);

  // --- messages from the exchange.
  void on_exchange(const wire::OutboundMessage& m, std::uint64_t ts);

  // --- session events.
  void on_disconnect();                // every in-flight request becomes uncertain
  void on_resent(const Token& token);  // request re-sent with its original token
  void on_cancel_resent(const Token& token);

  [[nodiscard]] const Order* find(const Token& token) const;
  [[nodiscard]] const std::deque<Order>& orders() const { return orders_; }
  // Orders with an unanswered request, in creation order.
  [[nodiscard]] const std::set<std::uint32_t>& unresolved() const { return unresolved_; }
  [[nodiscard]] SymbolExposure exposure(const Symbol& s) const;
  [[nodiscard]] const Stats& stats() const { return stats_; }

  // Recomputes exposure and bookkeeping from scratch. Returns an empty string
  // when everything is consistent, otherwise a description of the first problem.
  [[nodiscard]] std::string check_invariants() const;

 private:
  struct FillKey {
    std::uint32_t idx;
    std::uint64_t match;
    friend bool operator==(const FillKey&, const FillKey&) = default;
  };
  struct FillKeyHash {
    std::size_t operator()(const FillKey& k) const noexcept {
      return std::hash<std::uint64_t>()(k.match * 0x9E3779B97F4A7C15ULL ^ k.idx);
    }
  };

  Order* lookup(const Token& token);
  Order& create(const wire::EnterOrder& req, std::uint32_t qty, std::uint64_t ts);
  void set_leaves(Order& o, std::uint32_t leaves);
  void sync(Order& o);  // maintain unresolved_
  void notify(OrderEvent e, const Order& o, std::uint32_t shares = 0, std::uint32_t price = 0);
  void clear_pending(Order& o);
  void abort_pending_replace(Order& o);
  void implicit_accept(Order& o);

  void apply(const wire::SystemEvent& m, std::uint64_t ts);
  void apply(const wire::Accepted& m, std::uint64_t ts);
  void apply(const wire::Replaced& m, std::uint64_t ts);
  void apply(const wire::Canceled& m, std::uint64_t ts);
  void apply(const wire::Executed& m, std::uint64_t ts);
  void apply(const wire::Rejected& m, std::uint64_t ts);

  std::deque<Order> orders_;
  std::unordered_map<Token, std::uint32_t, TokenHash> by_token_;
  std::unordered_map<Symbol, SymbolExposure, SymbolHash> exposure_;
  std::unordered_set<FillKey, FillKeyHash> fills_;
  std::set<std::uint32_t> unresolved_;
  Stats stats_;
  Listener listener_;
};

}  // namespace ouch
