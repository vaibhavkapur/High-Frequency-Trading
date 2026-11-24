// End-to-end tests: a real Gateway talking SoupBinTCP/OUCH over loopback TCP
// to the exchange simulator running on its own thread.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "ouch/gateway.hpp"
#include "ouch/sim/exchange.hpp"
#include "test_util.hpp"

using namespace ouch;
using sim::ExchangeSim;
using sim::FaultAction;
using sim::FaultConfig;
using sim::ScriptedFault;
using sim::SimConfig;

namespace {

constexpr std::uint32_t kPx = 1'500'000;  // $150.0000
const Symbol kSym("AAPL");

RiskLimits loose_limits() {
  RiskLimits l;
  l.max_order_qty = 100'000;
  l.max_notional = UINT64_MAX;
  l.require_reference_price = false;
  l.max_long = 1'000'000'000;
  l.max_short = 1'000'000'000;
  return l;
}

NewOrder order(char side, std::uint32_t shares, std::uint32_t price = kPx,
               std::uint32_t tif = kTifMarketHours) {
  NewOrder o;
  o.side = side;
  o.shares = shares;
  o.stock = kSym;
  o.price = price;
  o.tif = tif;
  return o;
}

template <typename Pred>
bool pump(Gateway& gw, Pred pred, int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    gw.poll(1);
  }
  return true;
}

void pump_for(Gateway& gw, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) gw.poll(1);
}

class Integration : public ::testing::Test {
 protected:
  std::unique_ptr<ExchangeSim> sim;
  std::unique_ptr<Gateway> gw;
  GatewayConfig gw_cfg;

  void start_sim(SimConfig sc = {}) {
    sim = std::make_unique<ExchangeSim>(std::move(sc));
    ASSERT_TRUE(sim->start());
    gw_cfg.port = sim->port();
  }
  void start_gateway(RiskLimits limits = loose_limits()) {
    gw_cfg.port = sim->port();
    gw = std::make_unique<Gateway>(gw_cfg, limits);
  }
  void login() {
    ASSERT_TRUE(gw->connect());
    ASSERT_TRUE(pump(*gw, [&] { return gw->active(); })) << gw->last_error();
  }
  void start(SimConfig sc = {}, RiskLimits limits = loose_limits()) {
    start_sim(std::move(sc));
    start_gateway(limits);
    login();
  }

  const Order& ord(const Token& t) {
    const Order* o = gw->orders().find(t);
    EXPECT_NE(o, nullptr);
    return *o;
  }
  // Everything sent has been answered and every sequenced message consumed.
  bool settled() {
    return gw->active() && gw->orders().unresolved().empty() &&
           gw->next_seq() - 1 == sim->sequenced_count(gw_cfg.username);
  }
  // Every journal record that mentions `token` (when the gateway has a journal).
  std::string trace(const Token& token) {
    if (gw_cfg.journal_path.empty()) return "";
    gw->flush_journal();
    std::vector<Record> records;
    EventLog::read_all(gw_cfg.journal_path, records);
    const std::string_view needle = token.raw();
    std::string out = "\n  journal trace:";
    for (const Record& r : records) {
      const std::string_view hay(reinterpret_cast<const char*>(r.payload.data()), r.payload.size());
      const bool session = r.kind == RecordKind::SessionUp || r.kind == RecordKind::SessionDown;
      if (!session && hay.find(needle) == std::string_view::npos) continue;
      out += "\n    ";
      out += static_cast<char>(r.kind);
      out += " seq=" + std::to_string(r.seq) + " ";
      if (session) continue;
      out += static_cast<char>(r.payload[0]);
      out += " ";
      for (std::size_t i = 1; i < r.payload.size(); ++i) {
        const char c = static_cast<char>(r.payload[i]);
        if (c >= 0x20 && c < 0x7F) {
          out += c;
        } else {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "<%02X>", r.payload[i]);
          out += hex;
        }
      }
    }
    return out;
  }

  // Everything needed to diagnose a run that failed to settle.
  std::string describe_stall() {
    std::string out = "session=" + std::to_string(static_cast<int>(gw->session())) +
                      " last_error='" + gw->last_error() + "'" +
                      " next_seq=" + std::to_string(gw->next_seq()) +
                      " sim_seq=" + std::to_string(sim->sequenced_count(gw_cfg.username)) +
                      " logins=" + std::to_string(gw->stats().logins) +
                      " disconnects=" + std::to_string(gw->stats().disconnects) +
                      " unresolved=" + std::to_string(gw->orders().unresolved().size());
    std::unordered_map<Token, sim::OrderTruth, TokenHash> truth;
    for (const auto& t : sim->truth(gw_cfg.username)) truth[t.token] = t;
    int shown = 0;
    for (std::uint32_t idx : gw->orders().unresolved()) {
      if (++shown > 8) break;
      const Order& o = gw->orders().orders()[idx];
      out += "\n  " + o.req.token.str() + " " + to_string(o.state) +
             " pending=" + std::to_string(static_cast<int>(o.pending)) +
             (o.pending_uncertain ? "?" : "") + " qty=" + std::to_string(o.qty) +
             " cum=" + std::to_string(o.cum_qty) + " leaves=" + std::to_string(o.leaves) +
             " cancel_to=" + std::to_string(o.cancel_to) +
             " credits=" + std::to_string(o.cancel_credits) + " replaces=" + o.replaces.str() +
             " pending_token=" + o.pending_token.str();
      if (auto it = truth.find(o.req.token); it != truth.end()) {
        out += " | exchange open=" + std::to_string(it->second.open) +
               " executed=" + std::to_string(it->second.executed) +
               (it->second.live ? " live" : " dead");
      } else {
        out += " | exchange: unknown token";
      }
    }
    return out;
  }

  bool wait_state(const Token& t, OrderState s) {
    return pump(*gw, [&] { return ord(t).state == s; });
  }

  // Compares every gateway order with the exchange's ground truth.
  void expect_matches_truth() {
    std::unordered_map<Token, sim::OrderTruth, TokenHash> truth;
    for (const auto& t : sim->truth(gw_cfg.username)) truth[t.token] = t;
    std::size_t known = 0;
    for (const Order& o : gw->orders().orders()) {
      auto it = truth.find(o.req.token);
      const std::uint32_t open = it == truth.end() ? 0 : it->second.open;
      const std::uint32_t executed = it == truth.end() ? 0 : it->second.executed;
      if (it != truth.end()) ++known;
      EXPECT_EQ(o.leaves, open) << o.req.token.str() << " " << to_string(o.state)
                                << trace(o.req.token);
      EXPECT_EQ(o.cum_qty, executed) << o.req.token.str() << " " << to_string(o.state);
      if (it != truth.end()) {
        EXPECT_EQ(!is_terminal(o.state), it->second.live) << o.req.token.str();
      }
    }
    EXPECT_EQ(known, truth.size()) << "exchange knows tokens the gateway does not";
    EXPECT_EQ(gw->orders().check_invariants(), "");
  }
};

}  // namespace

// ------------------------------------------------------------- happy paths

TEST_F(Integration, FullLifecycleOverTcp) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(buy.ok());
  EXPECT_EQ(buy.token.str(), "GW000000000001");
  EXPECT_EQ(ord(buy.token).state, OrderState::PendingNew);
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  EXPECT_NE(ord(buy.token).order_ref, 0u);
  EXPECT_GT(ord(buy.token).acked_ns, ord(buy.token).sent_ns);

  // A sell that crosses 40 shares of the resting buy.
  const auto sell = gw->submit(order('S', 40));
  ASSERT_TRUE(sell.ok());
  ASSERT_TRUE(wait_state(sell.token, OrderState::Filled));
  ASSERT_TRUE(wait_state(buy.token, OrderState::PartiallyFilled));
  EXPECT_EQ(ord(buy.token).cum_qty, 40u);
  EXPECT_EQ(ord(buy.token).leaves, 60u);
  EXPECT_EQ(ord(sell.token).notional, 40ull * kPx);
  EXPECT_EQ(gw->orders().exposure(kSym).position, 0);
  EXPECT_EQ(gw->orders().exposure(kSym).open_buy, 60u);

  ASSERT_TRUE(gw->cancel(buy.token).ok());
  EXPECT_EQ(ord(buy.token).pending, PendingRequest::Cancel);
  ASSERT_TRUE(wait_state(buy.token, OrderState::Canceled));
  EXPECT_EQ(ord(buy.token).cum_qty, 40u);
  EXPECT_EQ(gw->orders().exposure(kSym).open_buy, 0u);

  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  // Accepted, Accepted, Executed x2, Canceled.
  EXPECT_EQ(gw->next_seq(), 6u);
  expect_matches_truth();
  EXPECT_EQ(gw->orders().stats().duplicates, 0u);
  EXPECT_EQ(gw->orders().stats().anomalies, 0u);
}

TEST_F(Integration, PartialCancelKeepsOrderLive) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  ASSERT_TRUE(gw->cancel(buy.token, 30).ok());
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Live);
  EXPECT_EQ(ord(buy.token).leaves, 30u);
  // Reducing to the current size (or more) is refused locally.
  EXPECT_EQ(gw->cancel(buy.token, 30).status, SubmitStatus::InvalidState);
  expect_matches_truth();
}

TEST_F(Integration, IocWithoutLiquidityIsCanceledByExchange) {
  start();
  const auto r = gw->submit(order('B', 100, kPx, kTifIoc));
  ASSERT_TRUE(wait_state(r.token, OrderState::Canceled));
  EXPECT_EQ(ord(r.token).reason, 'I');
  EXPECT_EQ(ord(r.token).cum_qty, 0u);
  expect_matches_truth();
}

TEST_F(Integration, ReplaceChangesPriceAndSize) {
  start();
  const auto buy = gw->submit(order('B', 100, 1'490'000));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  const auto rep = gw->replace(buy.token, 200, 1'495'000);
  ASSERT_TRUE(rep.ok());
  EXPECT_NE(rep.token, buy.token);
  ASSERT_TRUE(wait_state(rep.token, OrderState::Live));
  EXPECT_EQ(ord(buy.token).state, OrderState::Replaced);
  EXPECT_EQ(ord(buy.token).replaced_by, rep.token);
  EXPECT_EQ(ord(rep.token).leaves, 200u);
  EXPECT_EQ(ord(rep.token).req.price, 1'495'000u);
  EXPECT_NE(ord(rep.token).order_ref, ord(buy.token).order_ref);
  EXPECT_EQ(gw->orders().exposure(kSym).open_buy, 200u);

  // The replacement trades at its new price; the old token is gone.
  const auto sell = gw->submit(order('S', 200, 1'495'000));
  ASSERT_TRUE(wait_state(rep.token, OrderState::Filled));
  ASSERT_TRUE(wait_state(sell.token, OrderState::Filled));
  EXPECT_EQ(gw->cancel(buy.token).status, SubmitStatus::InvalidState);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  expect_matches_truth();
}

TEST_F(Integration, ExchangeRejectsUnknownSymbol) {
  SimConfig sc;
  sc.symbols = {"AAPL", "MSFT"};
  start(sc);
  NewOrder o = order('B', 100);
  o.stock = Symbol("NOPE");
  const auto r = gw->submit(o);
  ASSERT_TRUE(r.ok());  // passed local checks; the exchange says no
  ASSERT_TRUE(wait_state(r.token, OrderState::Rejected));
  EXPECT_EQ(ord(r.token).reason, 'S');
  EXPECT_EQ(gw->orders().exposure(Symbol("NOPE")).open_buy, 0u);
  EXPECT_EQ(sim->stats().rejected, 1u);
}

TEST_F(Integration, ExchangeRejectsBadReplaceAndOriginalSurvives) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  const auto rep = gw->replace(buy.token, 200, kMaxLimitPrice + 1);  // invalid price
  ASSERT_TRUE(rep.ok());
  ASSERT_TRUE(wait_state(rep.token, OrderState::Rejected));
  EXPECT_EQ(ord(rep.token).reason, 'X');
  EXPECT_EQ(ord(buy.token).state, OrderState::Live);
  EXPECT_EQ(ord(buy.token).pending, PendingRequest::None);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  expect_matches_truth();
}

TEST_F(Integration, RiskRejectedOrdersNeverReachTheWire) {
  RiskLimits l;
  l.max_order_qty = 500;
  l.max_notional = 50'000ULL * kPriceScale;
  l.price_band_bps = 500;
  l.max_long = 600;
  l.max_short = 600;
  start({}, l);
  gw->risk().set_reference_price(kSym, kPx);

  EXPECT_EQ(gw->submit(order('B', 501)).risk, RiskReject::MaxOrderQty);
  EXPECT_EQ(gw->submit(order('B', 400)).risk, RiskReject::MaxNotional);  // $60,000
  EXPECT_EQ(gw->submit(order('B', 100, 1'600'000)).risk, RiskReject::PriceBand);
  EXPECT_EQ(gw->submit(order('B', 100, kMarketPrice)).risk, RiskReject::MarketOrderNotAllowed);
  NewOrder other = order('B', 100);
  other.stock = Symbol("MSFT");
  EXPECT_EQ(gw->submit(other).risk, RiskReject::NoReferencePrice);
  EXPECT_EQ(gw->submit(order('B', 0)).status, SubmitStatus::RiskRejected);
  EXPECT_TRUE(gw->orders().orders().empty());

  // Position limit counts resting orders: 300 + 300 open, a third would breach 600.
  const auto a = gw->submit(order('B', 300, 1'450'000));
  const auto b = gw->submit(order('B', 300, 1'450'000));
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(gw->submit(order('B', 1, 1'450'000)).risk, RiskReject::PositionLimit);
  // Growing an order through replace is checked too.
  ASSERT_TRUE(wait_state(a.token, OrderState::Live));
  EXPECT_EQ(gw->replace(a.token, 301, 1'450'000).risk, RiskReject::PositionLimit);

  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(gw->stats().risk_rejects, 8u);
  EXPECT_EQ(sim->stats().inbound_messages, 2u);  // only the two accepted orders were sent
  EXPECT_EQ(gw->orders().orders().size(), 2u);
}

TEST_F(Integration, RequestsAreRefusedWhileDisconnected) {
  start_sim();
  start_gateway();
  EXPECT_EQ(gw->submit(order('B', 100)).status, SubmitStatus::NotConnected);
  EXPECT_TRUE(gw->orders().orders().empty());
  EXPECT_EQ(gw->cancel(Token("NOPE")).status, SubmitStatus::UnknownOrder);
  EXPECT_EQ(gw->replace(Token("NOPE"), 1, 1).status, SubmitStatus::UnknownOrder);
}

TEST_F(Integration, CancelRacingFillResolvesToValidFinalState) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  // Sell and cancel go out back to back; the exchange sees the sell first, so
  // the fill wins and the cancel is silently ignored.
  const auto sell = gw->submit(order('S', 100));
  ASSERT_TRUE(gw->cancel(buy.token).ok());
  EXPECT_EQ(ord(buy.token).pending, PendingRequest::Cancel);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Filled);
  EXPECT_EQ(ord(buy.token).pending, PendingRequest::None);
  EXPECT_EQ(ord(sell.token).state, OrderState::Filled);
  EXPECT_EQ(sim->stats().ignored, 1u);
  expect_matches_truth();
}

TEST_F(Integration, CancelBeatsPartialFill) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  const auto sell = gw->submit(order('S', 30));
  ASSERT_TRUE(gw->cancel(buy.token).ok());
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Canceled);
  EXPECT_EQ(ord(buy.token).cum_qty, 30u);
  EXPECT_EQ(ord(buy.token).leaves, 0u);
  EXPECT_EQ(ord(sell.token).state, OrderState::Filled);
  expect_matches_truth();
}

TEST_F(Integration, TwoAccountsTradeWithEachOther) {
  SimConfig sc;
  sc.accounts = {{"user", "pass"}, {"mm", "mmpass"}};
  start(sc);
  GatewayConfig mm_cfg = gw_cfg;
  mm_cfg.username = "mm";
  mm_cfg.password = "mmpass";
  mm_cfg.token_prefix = "MM";
  Gateway mm(mm_cfg, loose_limits());
  ASSERT_TRUE(mm.connect());
  ASSERT_TRUE(pump(mm, [&] { return mm.active(); }));

  const auto ask = mm.submit(order('S', 100, 1'501'000));
  ASSERT_TRUE(pump(mm, [&] { return mm.orders().find(ask.token)->state == OrderState::Live; }));
  const auto buy = gw->submit(order('B', 60, 1'505'000));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Filled));
  ASSERT_TRUE(pump(mm, [&] { return mm.orders().find(ask.token)->cum_qty == 60; }));

  // Price improvement: the aggressor trades at the resting price.
  EXPECT_EQ(ord(buy.token).notional, 60ull * 1'501'000);
  EXPECT_EQ(gw->orders().exposure(kSym).position, 60);
  EXPECT_EQ(mm.orders().exposure(kSym).position, -60);
  EXPECT_EQ(mm.orders().find(ask.token)->state, OrderState::PartiallyFilled);
  // Sequence numbers are per account.
  EXPECT_EQ(gw->next_seq(), 3u);
  EXPECT_EQ(mm.next_seq(), 3u);
}

// ---------------------------------------------------------- session control

TEST_F(Integration, LoginRejectedForBadPassword) {
  start_sim();
  gw_cfg.password = "wrong";
  start_gateway();
  ASSERT_TRUE(gw->connect());
  ASSERT_TRUE(pump(*gw, [&] { return gw->session() == Gateway::Session::Closed; }));
  EXPECT_EQ(gw->last_error(), "login rejected: not authorized");
  EXPECT_EQ(sim->stats().login_rejects, 1u);
  // A rejected login is final: no reconnect storm.
  pump_for(*gw, 200);
  EXPECT_EQ(sim->stats().login_rejects, 1u);
}

TEST_F(Integration, ConnectFailureIsReportedAndRetried) {
  start_sim();
  const std::uint16_t port = sim->port();
  sim->stop();  // nothing is listening on this port now
  start_gateway();
  EXPECT_FALSE(gw->connect());
  EXPECT_EQ(gw->session(), Gateway::Session::Disconnected);

  // Bring an exchange up on that port; the gateway's retry loop finds it.
  SimConfig sc;
  sc.port = port;
  sim = std::make_unique<ExchangeSim>(sc);
  ASSERT_TRUE(sim->start());
  ASSERT_TRUE(pump(*gw, [&] { return gw->active(); }));
}

TEST_F(Integration, HeartbeatsKeepAnIdleSessionAlive) {
  SimConfig sc;
  sc.heartbeat_interval_ms = 20;
  sc.client_timeout_ms = 150;
  start_sim(sc);
  gw_cfg.heartbeat_interval_ms = 20;
  gw_cfg.server_timeout_ms = 150;
  start_gateway();
  login();
  pump_for(*gw, 600);  // four timeout periods of total silence
  EXPECT_TRUE(gw->active());
  EXPECT_EQ(gw->stats().disconnects, 0u);
  EXPECT_GT(gw->stats().heartbeats_sent, 5u);
  EXPECT_GT(gw->stats().heartbeats_received, 5u);
}

TEST_F(Integration, SilentServerIsDetectedAndSessionRecovers) {
  SimConfig sc;
  sc.heartbeat_interval_ms = 20;
  start_sim(sc);
  gw_cfg.heartbeat_interval_ms = 20;
  gw_cfg.server_timeout_ms = 150;
  start_gateway();
  login();
  const auto live = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(live.token, OrderState::Live));

  sim->set_muted(true);
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().disconnects >= 1; }));
  EXPECT_EQ(gw->last_error(), "server heartbeat timeout");
  sim->set_muted(false);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(live.token).state, OrderState::Live);
  expect_matches_truth();
}

TEST_F(Integration, ExchangeDropsClientThatStopsHeartbeating) {
  SimConfig sc;
  sc.client_timeout_ms = 100;
  start_sim(sc);
  gw_cfg.heartbeat_interval_ms = 10'000;  // effectively never
  gw_cfg.auto_reconnect = false;
  start_gateway();
  login();
  ASSERT_TRUE(pump(*gw, [&] { return !gw->active(); }));
  EXPECT_EQ(gw->last_error(), "connection closed by peer");
}

TEST_F(Integration, ControlledShutdown) {
  start();
  const auto live = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(live.token, OrderState::Live));
  gw->logout();
  EXPECT_EQ(gw->session(), Gateway::Session::Closed);
  EXPECT_EQ(gw->submit(order('B', 1)).status, SubmitStatus::NotConnected);
  pump_for(*gw, 100);  // no reconnect after a deliberate logout
  EXPECT_EQ(gw->session(), Gateway::Session::Closed);
  EXPECT_EQ(sim->stats().logins, 1u);
  // The acknowledged order is still known and still live at the exchange.
  EXPECT_EQ(ord(live.token).state, OrderState::Live);
}

// --------------------------------------------------- reconnect / reconcile

TEST_F(Integration, LostOrderEntersReconcilingAndIsAcceptedExactlyOnce) {
  start();
  sim->inject(ScriptedFault{FaultAction::DropAndDisconnect});
  const auto r = gw->submit(order('B', 100));
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().disconnects == 1; }));

  // The outcome is unknown: neither assumed dead nor assumed live.
  EXPECT_EQ(ord(r.token).state, OrderState::Reconciling);
  EXPECT_EQ(gw->orders().exposure(kSym).open_buy, 100u);
  EXPECT_EQ(gw->orders().stats().uncertain, 1u);
  EXPECT_EQ(gw->submit(order('B', 1)).status, SubmitStatus::NotConnected);

  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(r.token).state, OrderState::Live);
  EXPECT_EQ(ord(r.token).resends, 1u);
  EXPECT_EQ(gw->stats().resends, 1u);
  EXPECT_EQ(gw->stats().logins, 2u);
  EXPECT_EQ(sim->stats().accepted, 1u);
  EXPECT_EQ(sim->stats().duplicate_tokens, 0u);  // the first copy really was lost
  expect_matches_truth();
}

TEST_F(Integration, AcceptedButUnacknowledgedOrderIsNotDuplicatedOnReconnect) {
  start();
  // Resting liquidity so the uncertain order would trade twice if duplicated.
  const auto ask = gw->submit(order('S', 500));
  ASSERT_TRUE(wait_state(ask.token, OrderState::Live));

  sim->inject(ScriptedFault{FaultAction::ProcessThenDisconnect});
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().disconnects == 1; }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Reconciling);
  EXPECT_EQ(sim->stats().accepted, 2u);  // the exchange did process it

  // The replay can settle the order before the exchange has even read the
  // resend, so wait for both.
  ASSERT_TRUE(pump(*gw, [&] { return settled() && sim->stats().duplicate_tokens == 1; }));
  // The sequenced replay delivered the Accepted and the fill; the same-token
  // resend was recognised and ignored by the exchange.
  EXPECT_EQ(ord(buy.token).state, OrderState::Filled);
  EXPECT_EQ(ord(buy.token).cum_qty, 100u);
  EXPECT_EQ(ord(ask.token).cum_qty, 100u);
  EXPECT_EQ(sim->stats().accepted, 2u);
  EXPECT_EQ(sim->stats().duplicate_tokens, 1u);
  EXPECT_EQ(gw->orders().exposure(kSym).position, 0);
  expect_matches_truth();
}

TEST_F(Integration, LostCancelIsResent) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  sim->inject(ScriptedFault{FaultAction::DropAndDisconnect});
  ASSERT_TRUE(gw->cancel(buy.token).ok());
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().disconnects == 1; }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Live);
  EXPECT_TRUE(ord(buy.token).pending_uncertain);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Canceled);
  EXPECT_EQ(sim->stats().cancels, 1u);
  expect_matches_truth();
}

TEST_F(Integration, LostReplaceIsResentWithItsOriginalReplacementToken) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  sim->inject(ScriptedFault{FaultAction::DropAndDisconnect});
  const auto rep = gw->replace(buy.token, 300, 1'490'000);
  ASSERT_TRUE(rep.ok());
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().disconnects == 1; }));
  EXPECT_EQ(ord(rep.token).state, OrderState::Reconciling);
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(rep.token).state, OrderState::Live);
  EXPECT_EQ(ord(rep.token).leaves, 300u);
  EXPECT_EQ(ord(buy.token).state, OrderState::Replaced);
  EXPECT_EQ(sim->stats().replaces, 1u);
  expect_matches_truth();
}

TEST_F(Integration, HoldPolicyLeavesUncertainOrdersForTheOwner) {
  start_sim();
  gw_cfg.reconcile = ReconcilePolicy::Hold;
  start_gateway();
  login();
  sim->inject(ScriptedFault{FaultAction::DropAndDisconnect});
  const auto r = gw->submit(order('B', 100));
  ASSERT_TRUE(pump(*gw, [&] { return gw->stats().logins == 2 && gw->active(); }));
  pump_for(*gw, 100);
  EXPECT_EQ(ord(r.token).state, OrderState::Reconciling);
  EXPECT_EQ(gw->stats().resends, 0u);
  EXPECT_EQ(sim->stats().accepted, 0u);

  EXPECT_EQ(gw->resend_uncertain(), 1u);
  ASSERT_TRUE(wait_state(r.token, OrderState::Live));
  EXPECT_EQ(sim->stats().accepted, 1u);
}

TEST_F(Integration, MessagesMissedWhileDisconnectedAreReplayedOnLogin) {
  SimConfig sc;
  sc.accounts = {{"user", "pass"}, {"mm", "mmpass"}};
  start_sim(sc);
  gw_cfg.auto_reconnect = false;
  start_gateway();
  login();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  gw->drop_connection();
  EXPECT_EQ(gw->session(), Gateway::Session::Disconnected);

  // While we are away, another account trades against our resting order.
  GatewayConfig mm_cfg = gw_cfg;
  mm_cfg.username = "mm";
  mm_cfg.password = "mmpass";
  mm_cfg.token_prefix = "MM";
  Gateway mm(mm_cfg, loose_limits());
  ASSERT_TRUE(mm.connect());
  ASSERT_TRUE(pump(mm, [&] { return mm.active(); }));
  const auto sell = mm.submit(order('S', 100));
  ASSERT_TRUE(pump(mm, [&] { return mm.orders().find(sell.token)->state == OrderState::Filled; }));

  // Acknowledged orders are not in doubt, but we have not heard about the fill.
  EXPECT_EQ(ord(buy.token).state, OrderState::Live);
  EXPECT_EQ(ord(buy.token).cum_qty, 0u);

  login();  // asks for sequence number 2 onward
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(buy.token).state, OrderState::Filled);
  EXPECT_EQ(gw->stats().sequenced_received, 2u);  // Accepted earlier + the replayed Executed
  EXPECT_EQ(gw->orders().stats().duplicates, 0u);
  expect_matches_truth();
}

// --------------------------------------------------- injected wire faults

TEST_F(Integration, DuplicateResponsesAreNotDoubleCounted) {
  start();
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(buy.token, OrderState::Live));
  sim->inject(ScriptedFault{FaultAction::DuplicateResponses});
  const auto sell = gw->submit(order('S', 60));
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));

  // Accepted(sell) x2, Executed(sell) x2, Executed(buy) x2 were all delivered.
  EXPECT_EQ(sim->stats().injected_duplicates, 3u);
  EXPECT_EQ(gw->next_seq() - 1, 7u);
  EXPECT_EQ(gw->orders().stats().duplicates, 3u);
  EXPECT_EQ(ord(sell.token).cum_qty, 60u);
  EXPECT_EQ(ord(buy.token).cum_qty, 60u);
  EXPECT_EQ(ord(buy.token).leaves, 40u);
  EXPECT_EQ(gw->orders().exposure(kSym).position, 0);
  expect_matches_truth();
}

TEST_F(Integration, ExecutionDeliveredBeforeItsAccepted) {
  start();
  const auto ask = gw->submit(order('S', 100));
  ASSERT_TRUE(wait_state(ask.token, OrderState::Live));
  sim->inject(ScriptedFault{FaultAction::ReorderResponses});
  const auto buy = gw->submit(order('B', 100));
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));

  EXPECT_EQ(sim->stats().injected_reorders, 1u);
  EXPECT_EQ(gw->orders().stats().implicit_accepts, 1u);
  EXPECT_EQ(ord(buy.token).state, OrderState::Filled);
  EXPECT_EQ(ord(buy.token).cum_qty, 100u);
  EXPECT_NE(ord(buy.token).order_ref, 0u);  // learned from the late Accepted
  EXPECT_EQ(gw->orders().stats().duplicates, 0u);
  expect_matches_truth();
}

TEST_F(Integration, DelayedAcknowledgementKeepsOrderPending) {
  start();
  sim->inject(ScriptedFault{FaultAction::DelayResponses, 0, 150});
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = gw->submit(order('B', 100));
  pump_for(*gw, 50);
  EXPECT_EQ(ord(r.token).state, OrderState::PendingNew);
  EXPECT_EQ(gw->orders().exposure(kSym).open_buy, 100u);  // pending orders are exposure
  ASSERT_TRUE(wait_state(r.token, OrderState::Live));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_GE(elapsed, std::chrono::milliseconds(140));
  const Order& o = ord(r.token);
  EXPECT_GE(o.acked_ns - o.sent_ns, 140'000'000u);
}

// ------------------------------------------------------- journal recovery

TEST_F(Integration, RestartRecoversStateFromJournalAndReconciles) {
  testutil::TempFile journal("recovery");
  start_sim();
  gw_cfg.journal_path = journal.path();
  start_gateway();
  login();

  const auto a = gw->submit(order('B', 100));
  ASSERT_TRUE(wait_state(a.token, OrderState::Live));
  const auto b = gw->submit(order('S', 40));
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  // The next order is lost in transit, and then the gateway process "dies".
  sim->inject(ScriptedFault{FaultAction::DropAndDisconnect});
  const auto c = gw->submit(order('B', 10, 1'400'000));
  ASSERT_TRUE(c.ok());
  gw->flush_journal();
  gw.reset();

  // A new process: same journal, nothing else.
  start_gateway();
  EXPECT_EQ(gw->orders().orders().size(), 3u);
  EXPECT_EQ(ord(a.token).state, OrderState::PartiallyFilled);
  EXPECT_EQ(ord(a.token).cum_qty, 40u);
  EXPECT_EQ(ord(a.token).leaves, 60u);
  EXPECT_EQ(ord(b.token).state, OrderState::Filled);
  EXPECT_EQ(ord(c.token).state, OrderState::Reconciling);
  EXPECT_EQ(gw->orders().exposure(kSym).position, 0);
  EXPECT_EQ(gw->next_seq(), 5u);  // Accepted x2 + Executed x2 already consumed
  EXPECT_EQ(gw->orders().check_invariants(), "");

  login();
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  EXPECT_EQ(ord(c.token).state, OrderState::Live);
  // Only the one new message was read: no replay of history, no double counting.
  EXPECT_EQ(gw->stats().sequenced_received, 1u);
  EXPECT_EQ(ord(a.token).cum_qty, 40u);
  EXPECT_EQ(sim->stats().accepted, 3u);

  // Tokens continue where the previous process stopped.
  const auto d = gw->submit(order('B', 10, 1'400'000));
  EXPECT_EQ(d.token.str(), "GW000000000004");
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }));
  expect_matches_truth();
}

// ------------------------------------------------------------ fault storm

class FaultStorm : public Integration, public ::testing::WithParamInterface<std::uint64_t> {};

TEST_P(FaultStorm, GatewayConvergesToExchangeTruth) {
  const std::uint64_t seed = GetParam();
  SimConfig sc;
  sc.reorder_flush_ms = 2;
  sc.faults = FaultConfig::uniform(0.03, seed);
  sc.faults.delay_ms = 1;
  start_sim(sc);
  gw_cfg.reconnect_backoff_ms = 1;
  gw_cfg.reconnect_backoff_max_ms = 4;
  // Journal the run so a failure can show exactly what the gateway saw.
  testutil::TempFile journal("storm");
  gw_cfg.journal_path = journal.path();
  start_gateway();
  login();

  std::mt19937_64 rng(seed);
  const auto pick = [&](std::uint32_t lo, std::uint32_t hi) {
    return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
  };
  std::vector<Token> mine;
  std::set<Token> cancel_sent;

  for (int step = 0; step < 2500; ++step) {
    if (!gw->active()) {
      gw->poll(1);
      continue;
    }
    const std::uint32_t action = pick(0, 99);
    if (action < 55 || mine.empty()) {
      NewOrder o = order(pick(0, 1) == 0 ? 'B' : 'S', pick(1, 5) * 100,
                         kPx + (pick(0, 6) - 3) * 100, pick(0, 5) == 0 ? kTifIoc : kTifMarketHours);
      const auto r = gw->submit(o);
      if (r.ok()) mine.push_back(r.token);
    } else {
      // Bias toward recent orders, which are more likely to still be working.
      const std::size_t window = std::min<std::size_t>(mine.size(), 40);
      const Token t = mine[mine.size() - 1 - pick(0, static_cast<std::uint32_t>(window) - 1)];
      const Order& o = ord(t);
      if (action < 80) {
        // One cancel request per order (see docs/design.md, duplicate cancels).
        if (!is_terminal(o.state) && o.pending == PendingRequest::None &&
            !cancel_sent.contains(t)) {
          const std::uint32_t keep = pick(0, 2) == 0 ? o.leaves / 2 : 0;
          if (gw->cancel(t, keep).ok()) cancel_sent.insert(t);
        }
      } else if (o.working() && o.pending == PendingRequest::None) {
        const auto r = gw->replace(t, o.cum_qty + pick(1, 5) * 100, kPx + (pick(0, 6) - 3) * 100);
        if (r.ok()) mine.push_back(r.token);
      }
    }
    if (pick(0, 3) == 0) gw->poll(0);
  }

  sim->set_faults(FaultConfig{});
  ASSERT_TRUE(pump(*gw, [&] { return settled(); }, 30000)) << describe_stall();

  expect_matches_truth();
  const auto st = sim->stats();
  // The storm really did exercise every fault class.
  EXPECT_GT(st.injected_disconnects, 0u);
  EXPECT_GT(st.injected_duplicates, 0u);
  EXPECT_GT(st.injected_reorders, 0u);
  EXPECT_GT(st.injected_delays, 0u);
  EXPECT_GT(st.executions, 0u);
  EXPECT_GT(gw->stats().resends, 0u);
  EXPECT_GT(gw->orders().stats().duplicates, 0u);
}

INSTANTIATE_TEST_SUITE_P(Seeds, FaultStorm, ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8));
