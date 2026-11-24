#include "ouch/gateway.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace ouch {
namespace {

inline constexpr std::uint64_t kNsPerMs = 1'000'000;

void sleep_ms(int ms) {
  if (ms > 0) ::poll(nullptr, 0, ms);
}

}  // namespace

const char* to_string(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::Ok:
      return "ok";
    case SubmitStatus::NotConnected:
      return "not-connected";
    case SubmitStatus::RiskRejected:
      return "risk-rejected";
    case SubmitStatus::UnknownOrder:
      return "unknown-order";
    case SubmitStatus::InvalidState:
      return "invalid-state";
  }
  return "?";
}

Gateway::Gateway(GatewayConfig cfg, RiskLimits limits)
    : cfg_(std::move(cfg)), risk_(limits), backoff_ms_(cfg_.reconnect_backoff_ms) {
  if (cfg_.token_prefix.size() > 4) cfg_.token_prefix.resize(4);
  if (cfg_.journal_path.empty()) return;

  // Recover from an existing journal, then keep appending to it.
  std::vector<Record> records;
  if (EventLog::read_all(cfg_.journal_path, records) && !records.empty()) {
    const ReplayResult res = replay(records, oms_);
    next_seq_ = res.next_seq;
    if (res.session.size() == session_id_.c.size()) session_id_ = soup::SessionId(res.session);
    // Every token this gateway issued created exactly one order record.
    token_counter_ = oms_.orders().size();
    // Whatever was in flight when the previous process stopped is now in doubt.
    oms_.on_disconnect();
  }
  if (!journal_.open(cfg_.journal_path, cfg_.journal_flush_each)) {
    last_error_ = "cannot open journal " + cfg_.journal_path;
  }
}

Gateway::~Gateway() {
  if (conn_.is_open()) handle_disconnect("gateway destroyed", true);
  journal_.close();
}

Token Gateway::next_token() {
  ++token_counter_;
  std::array<char, 15> buf{};
  const int digits = 14 - static_cast<int>(cfg_.token_prefix.size());
  std::snprintf(buf.data(), buf.size(), "%s%0*llu", cfg_.token_prefix.c_str(), digits,
                static_cast<unsigned long long>(token_counter_));
  return Token(std::string_view(buf.data(), 14));
}

// ------------------------------------------------------------------ session

bool Gateway::connect() {
  if (state_ == Session::Active || state_ == Session::LoginSent) return true;
  started_ = true;
  state_ = Session::Disconnected;
  const int fd = net::tcp_connect(cfg_.host, cfg_.port, cfg_.tcp_nodelay);
  const std::uint64_t now = now_ns();
  if (fd < 0) {
    last_error_ = "connect failed";
    next_attempt_ns_ = now + backoff_ms_ * kNsPerMs;
    backoff_ms_ = std::min(backoff_ms_ * 2, cfg_.reconnect_backoff_max_ms);
    return false;
  }
  conn_ = net::Conn(fd);
  parser_.reset();
  close_request_ = Close::None;

  soup::LoginRequest login;
  login.username = soup::Username(cfg_.username);
  login.password = soup::Password(cfg_.password);
  login.session = session_id_;  // blank on the first login of the day
  login.seq = next_seq_;        // resume exactly where we stopped
  soup::append_login_request(conn_.out(), login);
  last_tx_ns_ = last_rx_ns_ = now;
  state_ = Session::LoginSent;
  if (!conn_.flush()) {
    handle_disconnect("send failed during login", false);
    return false;
  }
  return true;
}

void Gateway::logout() {
  if (conn_.is_open()) {
    soup::append_packet(conn_.out(), soup::kLogoutRequest);
    conn_.flush();
  }
  handle_disconnect("logout", true);
}

void Gateway::drop_connection() {
  if (conn_.is_open()) handle_disconnect("connection dropped locally", false);
}

void Gateway::request_close(Close c, const char* reason) {
  if (close_request_ == Close::Final) return;
  close_request_ = c;
  close_reason_ = reason;
}

void Gateway::handle_disconnect(const char* reason, bool final) {
  const bool was_active = state_ == Session::Active;
  conn_.close();
  parser_.reset();
  close_request_ = Close::None;
  last_error_ = reason;
  if (was_active) {
    ++stats_.disconnects;
    journal_.append(RecordKind::SessionDown, now_ns(), next_seq_);
    journal_.flush();
    // Requests still waiting for a response may or may not have reached the
    // exchange. Mark them rather than guess.
    oms_.on_disconnect();
  }
  state_ = final ? Session::Closed : Session::Disconnected;
  next_attempt_ns_ = now_ns() + backoff_ms_ * kNsPerMs;
  backoff_ms_ =
      std::min(std::max<std::uint32_t>(backoff_ms_, 1) * 2, cfg_.reconnect_backoff_max_ms);
}

void Gateway::poll(int timeout_ms) {
  if (state_ == Session::Closed) {
    sleep_ms(timeout_ms);
    return;
  }
  if (state_ == Session::Disconnected) {
    const std::uint64_t now = now_ns();
    if (started_ && cfg_.auto_reconnect && now >= next_attempt_ns_) connect();
    if (state_ == Session::Disconnected) {
      int wait = timeout_ms;
      if (started_ && cfg_.auto_reconnect && next_attempt_ns_ > now) {
        wait = std::min<int>(wait, static_cast<int>((next_attempt_ns_ - now) / kNsPerMs) + 1);
      }
      sleep_ms(wait);
      return;
    }
  }

  // Never sleep past the next heartbeat deadline.
  int wait = std::min<int>(timeout_ms, static_cast<int>(cfg_.heartbeat_interval_ms));
  pollfd p{};
  p.fd = conn_.fd();
  p.events = POLLIN;
  if (conn_.pending() > 0) p.events |= POLLOUT;
  const int rc = ::poll(&p, 1, std::max(wait, 0));
  if (rc > 0) {
    if ((p.revents & POLLOUT) != 0 && !conn_.flush())
      request_close(Close::Disconnect, "send failed");
    if ((p.revents & (POLLIN | POLLHUP | POLLERR)) != 0) read_socket();
  }
  if (close_request_ == Close::None) run_timers();
  if (close_request_ != Close::None) {
    handle_disconnect(close_reason_, close_request_ == Close::Final);
  }
}

void Gateway::read_socket() {
  std::array<std::uint8_t, 64 * 1024> buf;  // NOLINT(cppcoreguidelines-pro-type-member-init)
  for (int round = 0; round < 8 && close_request_ == Close::None; ++round) {
    const long n = conn_.read_some(buf);
    if (n < 0) {
      request_close(Close::Disconnect, "connection closed by peer");
      return;
    }
    if (n == 0) return;
    last_rx_ns_ = now_ns();
    parser_.feed(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
    char type = 0;
    std::span<const std::uint8_t> payload;
    for (;;) {
      const auto st = parser_.next(type, payload);
      if (st == soup::FrameParser::Status::NeedMore) break;
      if (st == soup::FrameParser::Status::Error) {
        request_close(Close::Disconnect, "malformed packet from server");
        return;
      }
      handle_packet(type, payload);
      if (close_request_ != Close::None) return;
    }
    if (static_cast<std::size_t>(n) < buf.size()) return;
  }
}

void Gateway::handle_packet(char type, std::span<const std::uint8_t> payload) {
  switch (type) {
    case soup::kSequencedData: {
      if (state_ != Session::Active) return;
      const std::uint64_t ts = now_ns();
      const std::uint64_t seq = next_seq_++;
      ++stats_.sequenced_received;
      // Journal before applying, even if we cannot decode it: the sequence
      // number is consumed either way.
      journal_.append(RecordKind::ExchangeMsg, ts, seq, payload);
      wire::OutboundMessage m;
      if (wire::decode_outbound(payload, m) != wire::DecodeError::None) {
        ++stats_.bad_messages;
        return;
      }
      oms_.on_exchange(m, ts);
      return;
    }
    case soup::kServerHeartbeat:
      ++stats_.heartbeats_received;
      return;
    case soup::kLoginAccepted: {
      soup::LoginAccepted m;
      if (state_ != Session::LoginSent || !soup::parse_login_accepted(payload, m)) {
        request_close(Close::Disconnect, "unexpected or malformed login accepted");
        return;
      }
      session_id_ = m.session;
      next_seq_ = m.seq;
      state_ = Session::Active;
      backoff_ms_ = cfg_.reconnect_backoff_ms;
      ++stats_.logins;
      const std::string_view sid = session_id_.raw();
      journal_.append(RecordKind::SessionUp, now_ns(), next_seq_,
                      std::span<const std::uint8_t>(
                          reinterpret_cast<const std::uint8_t*>(sid.data()), sid.size()));
      if (cfg_.reconcile == ReconcilePolicy::ResendSameToken) resend_uncertain();
      return;
    }
    case soup::kLoginRejected:
      request_close(Close::Final, !payload.empty() && static_cast<char>(payload[0]) == 'S'
                                      ? "login rejected: session not available"
                                      : "login rejected: not authorized");
      return;
    case soup::kEndOfSession:
      request_close(Close::Final, "end of session");
      return;
    case soup::kDebug:
    default:
      return;  // unknown packet types are ignored, as the framing allows
  }
}

void Gateway::run_timers() {
  const std::uint64_t now = now_ns();
  if (now - last_rx_ns_ >= cfg_.server_timeout_ms * kNsPerMs) {
    request_close(Close::Disconnect, "server heartbeat timeout");
    return;
  }
  if (state_ == Session::Active && now - last_tx_ns_ >= cfg_.heartbeat_interval_ms * kNsPerMs) {
    soup::append_packet(conn_.out(), soup::kClientHeartbeat);
    last_tx_ns_ = now;
    ++stats_.heartbeats_sent;
    if (!conn_.flush()) request_close(Close::Disconnect, "send failed");
  }
}

// ----------------------------------------------------------------- requests

void Gateway::send_request(const wire::InboundMessage& m, RecordKind kind, std::uint64_t ts) {
  std::array<std::uint8_t, wire::kMaxMessageSize> buf;  // NOLINT
  const std::size_t n = wire::encode(m, buf);
  const std::span<const std::uint8_t> bytes(buf.data(), n);
  // Write-ahead: the journal learns about the request before the wire does.
  journal_.append(kind, ts, 0, bytes);
  soup::append_packet(conn_.out(), soup::kUnsequencedData, bytes);
  last_tx_ns_ = ts;
  if (kind == RecordKind::RequestSent) {
    ++stats_.requests_sent;
  } else {
    ++stats_.resends;
  }
  // A failed send is handled by poll(); the request is already recorded as
  // in flight and will become uncertain when the session is torn down.
  if (!conn_.flush()) request_close(Close::Disconnect, "send failed");
  stats_.max_send_queue_bytes =
      std::max<std::uint64_t>(stats_.max_send_queue_bytes, conn_.pending());
}

SubmitResult Gateway::submit(const NewOrder& o) {
  SubmitResult res;
  if (state_ != Session::Active) {
    res.status = SubmitStatus::NotConnected;
    return res;
  }
  res.risk = risk_.check(o.side, o.stock, o.shares, o.price, oms_.exposure(o.stock));

  wire::EnterOrder e;
  e.side = o.side;
  e.shares = o.shares;
  e.stock = o.stock;
  e.price = o.price;
  e.tif = o.tif;
  e.firm = cfg_.firm;
  e.display = o.display;
  e.capacity = o.capacity;
  e.iso = o.iso;
  e.min_qty = o.min_qty;
  e.cross_type = o.cross_type;
  e.customer_type = o.customer_type;

  const std::uint64_t ts = now_ns();
  if (res.risk != RiskReject::None) {
    res.status = SubmitStatus::RiskRejected;
    ++stats_.risk_rejects;
    if (journal_.is_open()) {
      std::array<std::uint8_t, 1 + wire::EnterOrder::kSize> buf;  // NOLINT
      buf[0] = static_cast<std::uint8_t>(res.risk);
      wire::encode(e, std::span<std::uint8_t>(buf).subspan(1));
      journal_.append(RecordKind::RiskReject, ts, 0, buf);
    }
    return res;
  }

  e.token = next_token();
  res.token = e.token;
  oms_.on_enter_sent(e, ts);
  send_request(e, RecordKind::RequestSent, ts);
  return res;
}

SubmitResult Gateway::cancel(const Token& token, std::uint32_t remaining) {
  SubmitResult res;
  res.token = token;
  const Order* o = oms_.find(token);
  if (o == nullptr) {
    res.status = SubmitStatus::UnknownOrder;
  } else if (is_terminal(o->state) || o->pending == PendingRequest::Replace ||
             remaining >= o->leaves) {
    res.status = SubmitStatus::InvalidState;  // includes "nothing to reduce"
  } else if (state_ != Session::Active) {
    res.status = SubmitStatus::NotConnected;
  }
  if (!res.ok()) return res;

  const wire::CancelOrder x{token, remaining};
  const std::uint64_t ts = now_ns();
  oms_.on_cancel_sent(x, ts);
  send_request(x, RecordKind::RequestSent, ts);
  return res;
}

SubmitResult Gateway::replace(const Token& token, std::uint32_t shares, std::uint32_t price) {
  SubmitResult res;
  res.token = token;
  const Order* o = oms_.find(token);
  if (o == nullptr) {
    res.status = SubmitStatus::UnknownOrder;
  } else if (!o->working() || o->pending != PendingRequest::None || shares <= o->cum_qty) {
    res.status = SubmitStatus::InvalidState;
  } else if (state_ != Session::Active) {
    res.status = SubmitStatus::NotConnected;
  }
  if (!res.ok()) return res;

  // The exchange swaps the two orders atomically, so the pair can never have
  // more open than the larger of them: check the replacement against the
  // symbol's exposure without the order it replaces.
  SymbolExposure exposure = oms_.exposure(o->req.stock);
  std::uint64_t& open = is_buy(o->req.side) ? exposure.open_buy : exposure.open_sell;
  open -= std::min<std::uint64_t>(open, o->leaves);
  res.risk = risk_.check(o->req.side, o->req.stock, shares - o->cum_qty, price, exposure);
  if (res.risk != RiskReject::None) {
    res.status = SubmitStatus::RiskRejected;
    ++stats_.risk_rejects;
    return res;
  }

  wire::ReplaceOrder u;
  u.existing_token = token;
  u.replacement_token = next_token();
  u.shares = shares;
  u.price = price;
  u.tif = o->req.tif;
  u.display = o->req.display;
  u.iso = o->req.iso;
  u.min_qty = 0;
  res.token = u.replacement_token;
  const std::uint64_t ts = now_ns();
  oms_.on_replace_sent(u, ts);
  send_request(u, RecordKind::RequestSent, ts);
  return res;
}

std::size_t Gateway::resend_uncertain() {
  if (state_ != Session::Active) return 0;
  // Creation order, so an order is always re-entered before its own cancel.
  const std::vector<std::uint32_t> idxs(oms_.unresolved().begin(), oms_.unresolved().end());
  std::size_t sent = 0;
  for (std::uint32_t idx : idxs) {
    const Order& o = oms_.orders()[idx];
    const std::uint64_t ts = now_ns();
    if (o.state == OrderState::Reconciling) {
      if (o.replaces.blank()) {
        wire::EnterOrder e = o.req;
        send_request(e, RecordKind::RequestResent, ts);
      } else {
        wire::ReplaceOrder u;
        u.existing_token = o.replaces;
        u.replacement_token = o.req.token;
        u.shares = o.replace_shares;
        u.price = o.req.price;
        u.tif = o.req.tif;
        u.display = o.req.display;
        u.iso = o.req.iso;
        u.min_qty = o.req.min_qty;
        send_request(u, RecordKind::RequestResent, ts);
      }
      oms_.on_resent(o.req.token);
      ++sent;
    }
    if (o.pending == PendingRequest::Cancel && o.pending_uncertain) {
      send_request(wire::CancelOrder{o.req.token, o.cancel_to}, RecordKind::RequestResent, ts);
      oms_.on_cancel_resent(o.req.token);
      ++sent;
    }
  }
  return sent;
}

}  // namespace ouch
