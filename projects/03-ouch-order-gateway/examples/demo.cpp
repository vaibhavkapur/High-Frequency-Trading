// End-to-end walkthrough: starts the exchange simulator in-process, connects a
// gateway over loopback TCP and narrates one of everything: order entry,
// partial fill, replace, cancel, risk rejects, exchange reject, a lost order
// reconciled after reconnect, duplicate responses, and restart from the journal.
//
// Usage: ouch_demo [journal-path]     (default: ./ouch_demo.journal, recreated each run)
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "ouch/gateway.hpp"
#include "ouch/sim/exchange.hpp"

using namespace ouch;

namespace {

std::string px(std::uint32_t price) {
  if (price == kMarketPrice) return "MKT";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u.%04u", price / kPriceScale, price % kPriceScale);
  return buf;
}

void print_update(const OrderUpdate& u) {
  const Order& o = *u.order;
  std::printf("    <- %-16s %s  %-15s qty=%-4u cum=%-4u leaves=%-4u", to_string(u.event),
              o.req.token.str().c_str(), to_string(o.state), o.qty, o.cum_qty, o.leaves);
  if (u.event == OrderEvent::Executed) std::printf("  fill %u @ %s", u.shares, px(u.price).c_str());
  if (u.event == OrderEvent::Canceled) {
    std::printf("  -%u", u.shares);
    if (o.state == OrderState::Canceled) std::printf(" (reason %c)", o.reason);
  }
  if (u.event == OrderEvent::Rejected) {
    std::printf("  reason %c%s", o.reason, o.locally_aborted ? " (local)" : "");
  }
  std::printf("\n");
}

template <typename Pred>
bool pump(Gateway& gw, Pred pred, int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    gw.poll(1);
  }
  return true;
}

void step(const char* title) { std::printf("\n== %s\n", title); }

NewOrder order(char side, std::uint32_t shares, std::uint32_t price, const char* sym = "AAPL") {
  NewOrder o;
  o.side = side;
  o.shares = shares;
  o.stock = Symbol(sym);
  o.price = price;
  return o;
}

SubmitResult show(const char* what, const SubmitResult& r) {
  if (r.ok()) {
    std::printf("    -> %-24s token %s\n", what, r.token.str().c_str());
  } else if (r.status == SubmitStatus::RiskRejected) {
    std::printf("    xx %-24s blocked by risk: %s\n", what, to_string(r.risk));
  } else {
    std::printf("    xx %-24s refused: %s\n", what, to_string(r.status));
  }
  return r;
}

void print_orders(const Gateway& gw) {
  std::printf("    %-15s %-4s %-8s %-10s %-15s %5s %5s %6s\n", "token", "side", "symbol", "price",
              "state", "qty", "cum", "leaves");
  for (const Order& o : gw.orders().orders()) {
    std::printf("    %-15s %-4c %-8s %-10s %-15s %5u %5u %6u\n", o.req.token.str().c_str(),
                o.req.side, o.req.stock.str().c_str(), px(o.req.price).c_str(), to_string(o.state),
                o.qty, o.cum_qty, o.leaves);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string journal = argc > 1 ? argv[1] : "ouch_demo.journal";
  std::remove(journal.c_str());

  sim::SimConfig sc;
  sc.symbols = {"AAPL", "MSFT"};
  sim::ExchangeSim exchange(sc);
  if (!exchange.start()) {
    std::fprintf(stderr, "cannot start exchange simulator\n");
    return 1;
  }
  std::printf("exchange simulator listening on 127.0.0.1:%u\n", exchange.port());

  GatewayConfig gc;
  gc.port = exchange.port();
  gc.journal_path = journal;
  RiskLimits limits;
  limits.max_order_qty = 1'000;
  limits.price_band_bps = 500;  // +/- 5%
  limits.max_long = 2'000;
  limits.max_short = 2'000;

  const auto make_gateway = [&] {
    auto gw = std::make_unique<Gateway>(gc, limits);
    gw->risk().set_reference_price(Symbol("AAPL"), 1'500'000);
    gw->risk().set_reference_price(Symbol("NOPE"), 1'500'000);
    gw->set_listener(print_update);
    return gw;
  };
  auto gw = make_gateway();
  const auto settled = [&] {
    return gw->active() && gw->orders().unresolved().empty() &&
           gw->next_seq() - 1 == exchange.sequenced_count(gc.username);
  };

  step("1. login");
  gw->connect();
  if (!pump(*gw, [&] { return gw->active(); })) {
    std::fprintf(stderr, "login failed: %s\n", gw->last_error().c_str());
    return 1;
  }
  std::printf("    session active, next sequence number %llu\n",
              static_cast<unsigned long long>(gw->next_seq()));

  step("2. enter a resting buy, then a sell that partially fills it");
  const auto buy = show("buy 100 AAPL @ 150.00", gw->submit(order('B', 100, 1'500'000)));
  pump(*gw, settled);
  show("sell 40 AAPL @ 150.00", gw->submit(order('S', 40, 1'500'000)));
  pump(*gw, settled);

  step("3. replace the remainder: new price, larger size (new token, old one retired)");
  const auto rep = show("replace -> 200 @ 149.50", gw->replace(buy.token, 200, 1'495'000));
  pump(*gw, settled);

  step("4. partial cancel down to 50 open shares, then cancel the rest");
  show("cancel to 50", gw->cancel(rep.token, 50));
  pump(*gw, settled);
  show("cancel", gw->cancel(rep.token));
  pump(*gw, settled);

  step("5. pre-trade risk: these never reach the wire");
  show("buy 5000 (max qty 1000)", gw->submit(order('B', 5'000, 1'500'000)));
  show("buy 100 @ 170.00 (band 5%)", gw->submit(order('B', 100, 1'700'000)));
  show("buy 100 MSFT (no ref px)", gw->submit(order('B', 100, 4'000'000, "MSFT")));
  show("market order", gw->submit(order('B', 100, kMarketPrice)));
  std::printf("    exchange has seen %llu messages so far; %llu requests blocked locally\n",
              static_cast<unsigned long long>(exchange.stats().inbound_messages),
              static_cast<unsigned long long>(gw->stats().risk_rejects));

  step("6. exchange-side reject: passes local risk, unknown symbol at the exchange");
  show("buy 100 NOPE @ 150.00", gw->submit(order('B', 100, 1'500'000, "NOPE")));
  pump(*gw, settled);

  step("7. fault: order lost in transit, connection drops -> Reconciling -> resent once");
  const std::uint64_t accepted_before = exchange.stats().accepted;
  exchange.inject(sim::ScriptedFault{sim::FaultAction::DropAndDisconnect});
  const auto lost = show("buy 100 AAPL @ 149.00", gw->submit(order('B', 100, 1'490'000)));
  pump(*gw, [&] { return gw->stats().disconnects == 1; });
  std::printf("    !! disconnected (%s); order is %s, still counted as exposure\n",
              gw->last_error().c_str(), to_string(gw->orders().find(lost.token)->state));
  pump(*gw, settled);
  std::printf("    reconnected; exchange accepted it %llu time(s)\n",
              static_cast<unsigned long long>(exchange.stats().accepted - accepted_before));

  step("8. fault: exchange accepts and fills, but the connection drops before the ack");
  exchange.inject(sim::ScriptedFault{sim::FaultAction::ProcessThenDisconnect});
  const auto unacked = show("sell 100 AAPL @ 149.00", gw->submit(order('S', 100, 1'490'000)));
  pump(*gw, [&] { return gw->stats().disconnects == 2; });
  std::printf("    !! disconnected; order is %s\n",
              to_string(gw->orders().find(unacked.token)->state));
  pump(*gw, [&] { return settled() && exchange.stats().duplicate_tokens == 1; });
  std::printf(
      "    replay delivered the missed messages; the same-token resend was ignored\n"
      "    by the exchange (duplicate tokens ignored: %llu)\n",
      static_cast<unsigned long long>(exchange.stats().duplicate_tokens));

  step("9. fault: every response duplicated -> quantities counted once");
  show("buy 100 AAPL @ 150.00", gw->submit(order('B', 100, 1'500'000)));
  pump(*gw, settled);
  exchange.inject(sim::ScriptedFault{sim::FaultAction::DuplicateResponses});
  show("sell 100 AAPL @ 150.00", gw->submit(order('S', 100, 1'500'000)));
  pump(*gw, settled);
  std::printf("    duplicates dropped by the order manager: %llu, AAPL position: %lld\n",
              static_cast<unsigned long long>(gw->orders().stats().duplicates),
              static_cast<long long>(gw->orders().exposure(Symbol("AAPL")).position));

  step("10. restart: a new gateway rebuilds everything from the append-only journal");
  const auto open_order = show("buy 300 AAPL @ 148.00", gw->submit(order('B', 300, 1'480'000)));
  pump(*gw, settled);
  std::printf(
      "    before restart: %llu requests sent, %llu resends, %llu risk rejects, "
      "%llu logins\n",
      static_cast<unsigned long long>(gw->stats().requests_sent),
      static_cast<unsigned long long>(gw->stats().resends),
      static_cast<unsigned long long>(gw->stats().risk_rejects),
      static_cast<unsigned long long>(gw->stats().logins));
  const std::size_t orders_before = gw->orders().orders().size();
  const std::uint64_t seq_before = gw->next_seq();
  gw.reset();
  gw = make_gateway();
  std::printf("    recovered %zu/%zu orders, next sequence %llu (was %llu), invariants: %s\n",
              gw->orders().orders().size(), orders_before,
              static_cast<unsigned long long>(gw->next_seq()),
              static_cast<unsigned long long>(seq_before),
              gw->orders().check_invariants().empty() ? "ok" : "BROKEN");
  gw->connect();
  pump(*gw, settled);
  show("cancel recovered order", gw->cancel(open_order.token));
  pump(*gw, settled);

  step("11. controlled shutdown");
  gw->logout();
  std::printf("    session closed: %s\n", gw->last_error().c_str());

  step("final order table");
  print_orders(*gw);
  const auto& os = gw->orders().stats();
  const auto es = exchange.stats();
  std::printf("\n");
  std::printf("    oms     : %llu duplicates dropped, %llu uncertain, %llu anomalies\n",
              static_cast<unsigned long long>(os.duplicates),
              static_cast<unsigned long long>(os.uncertain),
              static_cast<unsigned long long>(os.anomalies));
  std::printf(
      "    exchange: %llu accepted, %llu rejected, %llu executions, %llu cancels, "
      "%llu replaces\n",
      static_cast<unsigned long long>(es.accepted), static_cast<unsigned long long>(es.rejected),
      static_cast<unsigned long long>(es.executions), static_cast<unsigned long long>(es.cancels),
      static_cast<unsigned long long>(es.replaces));

  // Cross-check the gateway's view against the exchange's ground truth.
  std::size_t mismatches = 0;
  for (const auto& t : exchange.truth(gc.username)) {
    const Order* o = gw->orders().find(t.token);
    if (o == nullptr || o->leaves != t.open || o->cum_qty != t.executed) ++mismatches;
  }
  std::printf("    gateway vs exchange ground truth: %zu mismatches\n", mismatches);
  std::printf("    journal: %s\n", journal.c_str());
  exchange.stop();
  return mismatches == 0 ? 0 : 1;
}
