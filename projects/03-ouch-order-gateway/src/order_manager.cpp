#include "ouch/order_manager.hpp"

#include <algorithm>
#include <variant>

namespace ouch {

const char* to_string(OrderState s) {
  switch (s) {
    case OrderState::PendingNew:
      return "PendingNew";
    case OrderState::Reconciling:
      return "Reconciling";
    case OrderState::Live:
      return "Live";
    case OrderState::PartiallyFilled:
      return "PartiallyFilled";
    case OrderState::Filled:
      return "Filled";
    case OrderState::Canceled:
      return "Canceled";
    case OrderState::Rejected:
      return "Rejected";
    case OrderState::Replaced:
      return "Replaced";
  }
  return "?";
}

const char* to_string(OrderEvent e) {
  switch (e) {
    case OrderEvent::Submitted:
      return "Submitted";
    case OrderEvent::CancelRequested:
      return "CancelRequested";
    case OrderEvent::ReplaceRequested:
      return "ReplaceRequested";
    case OrderEvent::Accepted:
      return "Accepted";
    case OrderEvent::Rejected:
      return "Rejected";
    case OrderEvent::Executed:
      return "Executed";
    case OrderEvent::Canceled:
      return "Canceled";
    case OrderEvent::Replaced:
      return "Replaced";
    case OrderEvent::Uncertain:
      return "Uncertain";
    case OrderEvent::Resent:
      return "Resent";
  }
  return "?";
}

// ------------------------------------------------------------------ helpers

Order* OrderManager::lookup(const Token& token) {
  auto it = by_token_.find(token);
  return it == by_token_.end() ? nullptr : &orders_[it->second];
}

const Order* OrderManager::find(const Token& token) const {
  auto it = by_token_.find(token);
  return it == by_token_.end() ? nullptr : &orders_[it->second];
}

SymbolExposure OrderManager::exposure(const Symbol& s) const {
  auto it = exposure_.find(s);
  return it == exposure_.end() ? SymbolExposure{} : it->second;
}

Order& OrderManager::create(const wire::EnterOrder& req, std::uint32_t qty, std::uint64_t ts) {
  Order o;
  o.idx = static_cast<std::uint32_t>(orders_.size());
  o.req = req;
  o.qty = qty;
  o.sent_ns = ts;
  orders_.push_back(o);
  by_token_.emplace(req.token, o.idx);
  Order& ref = orders_.back();
  set_leaves(ref, qty);
  return ref;
}

// The single place where open quantity changes, so per-symbol exposure can
// never drift from the per-order view.
void OrderManager::set_leaves(Order& o, std::uint32_t leaves) {
  if (leaves == o.leaves) return;
  SymbolExposure& e = exposure_[o.req.stock];
  std::uint64_t& open = is_buy(o.req.side) ? e.open_buy : e.open_sell;
  open = open - o.leaves + leaves;
  o.leaves = leaves;
}

void OrderManager::sync(Order& o) {
  // A pending cancel asks the exchange to leave at most cancel_to shares open.
  // Our open figure never understates the exchange's, so once an acknowledged
  // order is at or below the target the cancel has nothing left to do, and the
  // exchange stays silent about a cancel that removes nothing. Stop waiting.
  // (If the exchange did act on it first, its Canceled is still honoured: the
  // request's credit is kept.)
  if (o.pending == PendingRequest::Cancel && !is_unacked(o.state) && o.leaves <= o.cancel_to) {
    clear_pending(o);
  }
  const bool unresolved = is_unacked(o.state) || o.pending != PendingRequest::None;
  if (unresolved) {
    unresolved_.insert(o.idx);
  } else {
    unresolved_.erase(o.idx);
  }
}

void OrderManager::notify(OrderEvent e, const Order& o, std::uint32_t shares, std::uint32_t price) {
  if (listener_) listener_(OrderUpdate{e, &o, shares, price});
}

void OrderManager::clear_pending(Order& o) {
  o.pending = PendingRequest::None;
  o.pending_uncertain = false;
  o.pending_token = Token{};
}

// The order being replaced reached a terminal state before the replace was
// confirmed. The exchange ignores a replace for a dead order, so no response
// will come: terminate the replacement locally rather than leave it pending.
void OrderManager::abort_pending_replace(Order& o) {
  if (o.pending != PendingRequest::Replace) return;
  const Token replacement = o.pending_token;
  clear_pending(o);
  if (Order* r = lookup(replacement); r != nullptr && is_unacked(r->state)) {
    r->state = OrderState::Rejected;
    r->reason = kReasonLocalAbort;
    r->locally_aborted = true;
    clear_pending(*r);
    set_leaves(*r, 0);
    sync(*r);
    notify(OrderEvent::Rejected, *r);
  }
}

void OrderManager::implicit_accept(Order& o) {
  // An execution or cancel can only refer to an order the exchange accepted,
  // so its Accepted message was reordered or is still to come.
  ++stats_.implicit_accepts;
  o.state = OrderState::Live;
}

// ----------------------------------------------------------------- requests

bool OrderManager::on_enter_sent(const wire::EnterOrder& m, std::uint64_t ts) {
  if (by_token_.contains(m.token)) return false;
  Order& o = create(m, m.shares, ts);
  sync(o);
  notify(OrderEvent::Submitted, o);
  return true;
}

bool OrderManager::on_cancel_sent(const wire::CancelOrder& m, std::uint64_t /*ts*/) {
  Order* o = lookup(m.token);
  if (o == nullptr || is_terminal(o->state)) return false;
  if (o->pending == PendingRequest::Replace) return false;
  o->pending = PendingRequest::Cancel;
  o->pending_uncertain = false;
  o->cancel_to = m.shares;
  ++o->cancel_credits;
  sync(*o);
  notify(OrderEvent::CancelRequested, *o);
  return true;
}

bool OrderManager::on_replace_sent(const wire::ReplaceOrder& m, std::uint64_t ts) {
  Order* o = lookup(m.existing_token);
  if (o == nullptr || !o->working() || o->pending != PendingRequest::None) return false;
  if (by_token_.contains(m.replacement_token)) return false;

  wire::EnterOrder req = o->req;
  req.token = m.replacement_token;
  req.shares = m.shares;
  req.price = m.price;
  req.tif = m.tif;
  req.display = m.display;
  req.iso = m.iso;
  req.min_qty = m.min_qty;
  // The replace carries the new total size including what already executed;
  // the replacement order's open quantity is the remainder. This is an
  // estimate until the Replaced message reports the authoritative figure.
  const std::uint32_t open = m.shares > o->cum_qty ? m.shares - o->cum_qty : 0;
  const Token existing = o->req.token;

  Order& r = create(req, open, ts);  // may invalidate nothing: deque keeps references
  r.replaces = existing;
  r.replace_shares = m.shares;
  r.est_open = open;
  sync(r);

  o = lookup(existing);
  o->pending = PendingRequest::Replace;
  o->pending_uncertain = false;
  o->pending_token = m.replacement_token;
  sync(*o);
  notify(OrderEvent::ReplaceRequested, *o);
  return true;
}

bool OrderManager::on_request_sent(const wire::InboundMessage& m, std::uint64_t ts) {
  return std::visit(
      [&](const auto& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, wire::EnterOrder>) {
          return on_enter_sent(x, ts);
        } else if constexpr (std::is_same_v<T, wire::CancelOrder>) {
          return on_cancel_sent(x, ts);
        } else {
          return on_replace_sent(x, ts);
        }
      },
      m);
}

// ------------------------------------------------------------ session events

void OrderManager::on_disconnect() {
  // Copy: notify() listeners must not observe a set that is mid-iteration.
  const std::vector<std::uint32_t> idxs(unresolved_.begin(), unresolved_.end());
  for (std::uint32_t idx : idxs) {
    Order& o = orders_[idx];
    bool changed = false;
    if (o.state == OrderState::PendingNew) {
      o.state = OrderState::Reconciling;
      changed = true;
    }
    if (o.pending != PendingRequest::None && !o.pending_uncertain) {
      o.pending_uncertain = true;
      changed = true;
    }
    if (changed) {
      ++stats_.uncertain;
      notify(OrderEvent::Uncertain, o);
    }
  }
}

void OrderManager::on_resent(const Token& token) {
  Order* o = lookup(token);
  if (o == nullptr || !is_unacked(o->state)) return;
  ++o->resends;
  if (!o->replaces.blank()) {
    if (Order* p = lookup(o->replaces); p != nullptr && p->pending == PendingRequest::Replace) {
      p->pending_uncertain = false;
    }
  }
  notify(OrderEvent::Resent, *o);
}

void OrderManager::on_cancel_resent(const Token& token) {
  Order* o = lookup(token);
  if (o == nullptr || o->pending != PendingRequest::Cancel) return;
  o->pending_uncertain = false;
  notify(OrderEvent::Resent, *o);
}

// --------------------------------------------------------- exchange messages

void OrderManager::on_exchange(const wire::OutboundMessage& m, std::uint64_t ts) {
  std::visit([&](const auto& x) { apply(x, ts); }, m);
}

void OrderManager::apply(const wire::SystemEvent& /*m*/, std::uint64_t /*ts*/) {}

void OrderManager::apply(const wire::Accepted& m, std::uint64_t ts) {
  Order* o = lookup(m.token);
  if (o == nullptr) {
    ++stats_.unknown_tokens;
    return;
  }
  if (is_unacked(o->state)) {
    o->order_ref = m.order_ref;
    o->acked_ns = ts;
    o->qty = m.shares;
    o->req.price = m.price;
    if (m.order_state == 'D') {
      o->state = OrderState::Canceled;
      set_leaves(*o, 0);
      // A dead order cannot be canceled or replaced.
      abort_pending_replace(*o);
      clear_pending(*o);
    } else {
      o->state = OrderState::Live;
      set_leaves(*o, m.shares);
    }
    sync(*o);
    notify(OrderEvent::Accepted, *o);
    return;
  }
  if (o->order_ref == 0) {
    // Late Accepted for an order we already accepted implicitly: only learn
    // the reference number; quantities have moved on.
    o->order_ref = m.order_ref;
    o->acked_ns = ts;
    return;
  }
  ++stats_.duplicates;
}

void OrderManager::apply(const wire::Rejected& m, std::uint64_t /*ts*/) {
  Order* o = lookup(m.token);
  if (o == nullptr) {
    ++stats_.unknown_tokens;
    return;
  }
  if (!is_unacked(o->state)) {
    if (is_terminal(o->state)) {
      ++stats_.duplicates;
    } else {
      ++stats_.anomalies;  // a reject for an order the exchange already accepted
    }
    return;
  }
  o->state = OrderState::Rejected;
  o->reason = m.reason;
  set_leaves(*o, 0);
  clear_pending(*o);
  sync(*o);
  if (!o->replaces.blank()) {
    // Rejected replace: the original order is untouched and no longer pending.
    if (Order* p = lookup(o->replaces);
        p != nullptr && p->pending == PendingRequest::Replace && p->pending_token == m.token) {
      clear_pending(*p);
      sync(*p);
    }
  }
  notify(OrderEvent::Rejected, *o);
}

void OrderManager::apply(const wire::Replaced& m, std::uint64_t ts) {
  Order* r = lookup(m.token);
  if (r == nullptr) {
    ++stats_.unknown_tokens;
    return;
  }
  const bool revive = r->state == OrderState::Rejected && r->locally_aborted;
  if (is_unacked(r->state) || revive) {
    if (revive) {
      // We gave up on this replacement, but the exchange says it happened.
      ++stats_.revived;
      r->locally_aborted = false;
      r->reason = 0;
    }
    r->order_ref = m.order_ref;
    r->acked_ns = ts;
    r->req.price = m.price;
    // cum_qty is zero here except for a revived order that saw stray fills.
    set_leaves(*r, m.shares > r->cum_qty ? m.shares - r->cum_qty : 0);
    r->qty = r->cum_qty + r->leaves;
    r->state = r->cum_qty == 0
                   ? OrderState::Live
                   : (r->leaves == 0 ? OrderState::Filled : OrderState::PartiallyFilled);
  } else if (r->order_ref == 0) {
    // Late Replaced: an execution or cancel on the replacement overtook it and
    // the order was accepted implicitly at an estimated size. The message
    // reports the size the replacement really started with, so correct the
    // open quantity by the estimation error only: fills and cancels applied
    // since then already came off the estimate and must not be undone.
    r->order_ref = m.order_ref;
    r->acked_ns = ts;
    if (!is_terminal(r->state)) {
      const std::int64_t corrected = static_cast<std::int64_t>(r->leaves) +
                                     static_cast<std::int64_t>(m.shares) -
                                     static_cast<std::int64_t>(r->est_open);
      set_leaves(*r, corrected > 0 ? static_cast<std::uint32_t>(corrected) : 0);
      r->qty = r->cum_qty + r->leaves;
      if (r->leaves == 0) {
        r->state = r->cum_qty > 0 ? OrderState::Filled : OrderState::Canceled;
        clear_pending(*r);
      }
    }
  } else {
    ++stats_.duplicates;
    return;
  }
  sync(*r);

  if (Order* p = lookup(m.previous_token); p != nullptr && !is_terminal(p->state)) {
    p->state = OrderState::Replaced;
    p->replaced_by = m.token;
    set_leaves(*p, 0);
    clear_pending(*p);
    sync(*p);
  }
  notify(OrderEvent::Replaced, *r);
}

void OrderManager::apply(const wire::Canceled& m, std::uint64_t /*ts*/) {
  Order* o = lookup(m.token);
  if (o == nullptr) {
    ++stats_.unknown_tokens;
    return;
  }
  if (is_terminal(o->state)) {
    ++stats_.duplicates;
    return;
  }
  if (is_unacked(o->state)) implicit_accept(*o);

  // Canceled messages carry no unique id, so a duplicate of one that left the
  // order alive (a partial cancel) is recognised by context: it claims to be
  // user-requested, yet every cancel request we sent has been answered and no
  // replace (which the exchange may also answer with a cancel) is pending.
  if (m.reason == 'U') {
    if (o->cancel_credits == 0 && o->pending != PendingRequest::Replace) {
      ++stats_.duplicates;
      sync(*o);
      return;
    }
    if (o->cancel_credits > 0) --o->cancel_credits;
  }

  std::uint32_t dec = m.decrement_shares;
  if (dec > o->leaves) {
    ++stats_.anomalies;
    dec = o->leaves;
  }
  set_leaves(*o, o->leaves - dec);
  o->qty = o->cum_qty + o->leaves;
  if (o->pending == PendingRequest::Cancel) clear_pending(*o);
  if (o->leaves == 0) {
    o->state = OrderState::Canceled;
    o->reason = m.reason;
    abort_pending_replace(*o);
  }
  sync(*o);
  notify(OrderEvent::Canceled, *o, dec);
}

void OrderManager::apply(const wire::Executed& m, std::uint64_t /*ts*/) {
  Order* o = lookup(m.token);
  if (o == nullptr) {
    ++stats_.unknown_tokens;
    return;
  }
  if (m.shares == 0) {
    ++stats_.anomalies;
    return;
  }
  // Match numbers are unique per trade; (order, match) identifies one fill.
  if (!fills_.insert(FillKey{o->idx, m.match_number}).second) {
    ++stats_.duplicates;
    return;
  }
  if (is_unacked(o->state)) implicit_accept(*o);

  const bool was_terminal = is_terminal(o->state);
  if (was_terminal || m.shares > o->leaves) ++stats_.anomalies;

  // The exchange is the source of truth: an execution always counts toward
  // executed quantity and position, even when it surprises us.
  o->cum_qty += m.shares;
  o->notional += static_cast<std::uint64_t>(m.shares) * m.price;
  set_leaves(*o, m.shares >= o->leaves ? 0 : o->leaves - m.shares);
  SymbolExposure& e = exposure_[o->req.stock];
  e.position += is_buy(o->req.side) ? static_cast<std::int64_t>(m.shares)
                                    : -static_cast<std::int64_t>(m.shares);

  if (!was_terminal) {
    if (o->leaves == 0) {
      o->state = OrderState::Filled;
      o->qty = o->cum_qty;
      abort_pending_replace(*o);
      clear_pending(*o);  // a pending cancel lost the race to the fill
    } else {
      o->state = OrderState::PartiallyFilled;
    }
  }
  sync(*o);
  notify(OrderEvent::Executed, *o, m.shares, m.price);
}

// --------------------------------------------------------------- invariants

std::string OrderManager::check_invariants() const {
  std::unordered_map<Symbol, SymbolExposure, SymbolHash> expect;
  for (const Order& o : orders_) {
    const std::string tok = o.req.token.str();
    if (is_terminal(o.state)) {
      if (o.leaves != 0) return "terminal order with open shares: " + tok;
      if (o.pending != PendingRequest::None) return "terminal order with pending request: " + tok;
    }
    if (o.working() && o.qty != o.cum_qty + o.leaves) {
      return "qty != cum + leaves: " + tok;
    }
    if (o.state == OrderState::Filled && o.cum_qty == 0) return "filled with no fills: " + tok;
    if (o.state == OrderState::Live && o.cum_qty != 0) return "live with fills: " + tok;
    const bool unresolved = is_unacked(o.state) || o.pending != PendingRequest::None;
    if (unresolved != unresolved_.contains(o.idx)) return "unresolved set mismatch: " + tok;

    SymbolExposure& e = expect[o.req.stock];
    if (is_buy(o.req.side)) {
      e.open_buy += o.leaves;
      e.position += o.cum_qty;
    } else {
      e.open_sell += o.leaves;
      e.position -= o.cum_qty;
    }
  }
  for (const auto& [sym, e] : expect) {
    if (!(exposure(sym) == e)) return "exposure mismatch for " + sym.str();
  }
  for (const auto& [sym, e] : exposure_) {
    if (!expect.contains(sym) && !(e == SymbolExposure{})) {
      return "exposure for symbol with no orders: " + sym.str();
    }
  }
  return {};
}

}  // namespace ouch
