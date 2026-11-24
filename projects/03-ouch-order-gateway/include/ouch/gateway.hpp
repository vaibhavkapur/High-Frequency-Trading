// Order gateway: risk checks -> journal -> OMS -> SoupBinTCP session.
//
// Single-threaded and poll-driven: the owner calls poll() in a loop and all
// callbacks fire on that thread. Nothing here blocks except the initial TCP
// connect (bounded by the OS for a local/refused address).
#pragma once

#include <cstdint>
#include <string>

#include "ouch/event_log.hpp"
#include "ouch/order_manager.hpp"
#include "ouch/risk.hpp"
#include "ouch/socket.hpp"
#include "ouch/soupbin.hpp"
#include "ouch/types.hpp"

namespace ouch {

// What to do, after logging back in, with a request whose fate is unknown.
enum class ReconcilePolicy : std::uint8_t {
  // Re-send it with its ORIGINAL token. The exchange ignores a token it has
  // already seen, so the request takes effect at most once whether or not the
  // first copy arrived. Orders stay in Reconciling until a response arrives.
  ResendSameToken,
  // Do nothing automatically. Uncertain orders stay in Reconciling (and keep
  // counting as exposure) until the sequenced replay resolves them or the
  // owner calls resend_uncertain() / cancel().
  Hold,
};

struct GatewayConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string username = "user";
  std::string password = "pass";
  std::string token_prefix = "GW";  // at most 4 characters; must be unique per day per account
  Firm firm{"FIRM"};

  std::uint32_t heartbeat_interval_ms = 1'000;  // send a heartbeat after this much tx silence
  std::uint32_t server_timeout_ms = 15'000;     // drop the session after this much rx silence
  bool auto_reconnect = true;
  std::uint32_t reconnect_backoff_ms = 50;  // doubles per failed attempt up to the max
  std::uint32_t reconnect_backoff_max_ms = 2'000;
  ReconcilePolicy reconcile = ReconcilePolicy::ResendSameToken;

  bool tcp_nodelay = true;

  // Empty = no journal. If the file already exists it is replayed on
  // construction to recover order state, sequence number and token counter.
  std::string journal_path;
  bool journal_flush_each = false;
};

struct NewOrder {
  char side = 'B';
  std::uint32_t shares = 0;
  Symbol stock;
  std::uint32_t price = 0;
  std::uint32_t tif = kTifMarketHours;
  char display = 'Y';
  char capacity = 'A';
  char iso = 'N';
  std::uint32_t min_qty = 0;
  char cross_type = 'N';
  char customer_type = ' ';
};

enum class SubmitStatus : std::uint8_t {
  Ok,
  NotConnected,  // no active session; nothing was sent or recorded
  RiskRejected,  // refused by pre-trade risk; nothing was sent
  UnknownOrder,  // cancel/replace for a token this gateway never issued
  InvalidState,  // cancel/replace not possible in the order's current state
};

const char* to_string(SubmitStatus s);

struct SubmitResult {
  SubmitStatus status = SubmitStatus::Ok;
  RiskReject risk = RiskReject::None;
  Token token;  // new order token (submit/replace) or the target token (cancel)
  [[nodiscard]] bool ok() const { return status == SubmitStatus::Ok; }
};

class Gateway {
 public:
  enum class Session : std::uint8_t { Disconnected, LoginSent, Active, Closed };

  struct Stats {
    std::uint64_t requests_sent = 0;
    std::uint64_t risk_rejects = 0;
    std::uint64_t resends = 0;
    std::uint64_t logins = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t heartbeats_received = 0;
    std::uint64_t sequenced_received = 0;
    std::uint64_t bad_messages = 0;
    std::uint64_t max_send_queue_bytes = 0;
  };

  explicit Gateway(GatewayConfig cfg, RiskLimits limits = {});
  ~Gateway();
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // One connection attempt: TCP connect and send the login request. Returns
  // false if the TCP connect failed. The session becomes Active once poll()
  // processes the Login Accepted packet.
  bool connect();
  // Drives socket I/O, heartbeats, timeouts and reconnects. Waits at most
  // `timeout_ms` for socket activity.
  void poll(int timeout_ms);
  // Sends a Logout Request and closes the session for good.
  void logout();
  // Abruptly closes the socket as if the network failed (for tests and demos).
  void drop_connection();

  SubmitResult submit(const NewOrder& o);
  // `remaining` is the open quantity to keep; 0 cancels the whole order.
  SubmitResult cancel(const Token& token, std::uint32_t remaining = 0);
  // `shares` is the new total size including shares already executed.
  SubmitResult replace(const Token& token, std::uint32_t shares, std::uint32_t price);
  // Re-sends every uncertain request with its original token. Returns the
  // number of requests re-sent. Called automatically under ResendSameToken.
  std::size_t resend_uncertain();

  void set_listener(OrderManager::Listener l) { oms_.set_listener(std::move(l)); }
  RiskEngine& risk() { return risk_; }
  [[nodiscard]] const OrderManager& orders() const { return oms_; }
  [[nodiscard]] Session session() const { return state_; }
  [[nodiscard]] bool active() const { return state_ == Session::Active; }
  // Sequence number of the next sequenced message expected from the exchange.
  [[nodiscard]] std::uint64_t next_seq() const { return next_seq_; }
  [[nodiscard]] const Stats& stats() const { return stats_; }
  [[nodiscard]] const std::string& last_error() const { return last_error_; }
  [[nodiscard]] std::size_t send_queue_bytes() const { return conn_.pending(); }
  void flush_journal() { journal_.flush(); }

 private:
  enum class Close : std::uint8_t { None, Disconnect, Final };

  Token next_token();
  void send_request(const wire::InboundMessage& m, RecordKind kind, std::uint64_t ts);
  void handle_packet(char type, std::span<const std::uint8_t> payload);
  void handle_disconnect(const char* reason, bool final);
  void request_close(Close c, const char* reason);
  void read_socket();
  void run_timers();

  GatewayConfig cfg_;
  RiskEngine risk_;
  OrderManager oms_;
  EventLog journal_;
  net::Conn conn_;
  soup::FrameParser parser_;

  Session state_ = Session::Disconnected;
  bool started_ = false;
  Close close_request_ = Close::None;
  const char* close_reason_ = "";
  soup::SessionId session_id_;
  std::uint64_t next_seq_ = 1;
  std::uint64_t token_counter_ = 0;
  std::uint64_t last_rx_ns_ = 0;
  std::uint64_t last_tx_ns_ = 0;
  std::uint64_t next_attempt_ns_ = 0;
  std::uint32_t backoff_ms_ = 0;
  Stats stats_;
  std::string last_error_;
};

}  // namespace ouch
