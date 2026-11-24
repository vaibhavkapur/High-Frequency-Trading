#include "ouch/sim/exchange.hpp"

#include <poll.h>

#include <algorithm>
#include <variant>

namespace ouch::sim {
namespace {

inline constexpr std::uint64_t kNsPerMs = 1'000'000;
inline constexpr std::uint64_t kNsPerDay = 86'400ULL * 1'000'000'000ULL;
inline constexpr int kMaxPollMs = 20;

std::uint64_t ns_since_midnight() { return wall_ns() % kNsPerDay; }

}  // namespace

ExchangeSim::ExchangeSim(SimConfig cfg, std::unique_ptr<MatchingEngine> engine)
    : cfg_(std::move(cfg)),
      engine_(engine ? std::move(engine) : std::make_unique<SimpleMatchingEngine>()),
      faults_(cfg_.faults),
      rng_(cfg_.faults.seed) {
  for (const auto& s : cfg_.symbols) symbols_.insert(Symbol(s));
  for (const auto& [user, pass] : cfg_.accounts) accounts_[user] = pass;
  cfg_.session_id.resize(10, ' ');
}

ExchangeSim::~ExchangeSim() { stop(); }

bool ExchangeSim::start() {
  if (thread_.joinable()) return true;
  listen_fd_ = net::tcp_listen(cfg_.port, port_);
  if (listen_fd_ < 0) return false;
  stop_ = false;
  thread_ = std::thread([this] { run(); });
  return true;
}

void ExchangeSim::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mu_);
  clients_.clear();
  net::close_fd(listen_fd_);
}

// ------------------------------------------------------------------ controls

void ExchangeSim::set_faults(const FaultConfig& f) {
  std::lock_guard<std::mutex> lock(mu_);
  faults_ = f;
  rng_.seed(f.seed);
}

void ExchangeSim::inject(const ScriptedFault& f) {
  std::lock_guard<std::mutex> lock(mu_);
  script_.push_back(f);
}

void ExchangeSim::kick_all() {
  std::lock_guard<std::mutex> lock(mu_);
  for (Client& c : clients_) c.dead = true;
}

void ExchangeSim::set_muted(bool muted) {
  std::lock_guard<std::mutex> lock(mu_);
  muted_ = muted;
}

SimStats ExchangeSim::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

std::vector<OrderTruth> ExchangeSim::truth(const std::string& user) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<OrderTruth> out;
  auto it = sessions_.find(user);
  if (it == sessions_.end()) return out;
  out.reserve(it->second->orders.size());
  for (const auto& [token, o] : it->second->orders) {
    out.push_back(OrderTruth{token, o.open, o.executed, o.live});
  }
  return out;
}

std::uint64_t ExchangeSim::sequenced_count(const std::string& user) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(user);
  if (it == sessions_.end()) return 0;
  return it->second->log.size() + (it->second->held ? 1 : 0);
}

bool ExchangeSim::roll(double p) {
  if (p <= 0.0) return false;
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
}

// ---------------------------------------------------------------- event loop

int ExchangeSim::next_timeout_ms(std::uint64_t now) const {
  std::uint64_t deadline = now + kMaxPollMs * kNsPerMs;
  for (const auto& [user, s] : sessions_) {
    if (s->held) deadline = std::min(deadline, s->held_deadline_ns);
  }
  for (const Client& c : clients_) {
    if (c.session != nullptr && c.cursor < c.session->log.size()) {
      deadline = std::min(deadline, c.session->log[c.cursor].release_ns);
    }
  }
  if (deadline <= now) return 0;
  // Round up so we do not spin just short of the deadline.
  return static_cast<int>((deadline - now + kNsPerMs - 1) / kNsPerMs);
}

void ExchangeSim::run() {
  std::vector<pollfd> fds;
  std::vector<Client*> polled;
  while (!stop_) {
    int timeout = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      fds.clear();
      polled.clear();
      fds.push_back(pollfd{listen_fd_, POLLIN, 0});
      for (Client& c : clients_) {
        short events = POLLIN;
        if (c.conn.pending() > 0) events |= POLLOUT;
        fds.push_back(pollfd{c.conn.fd(), events, 0});
        polled.push_back(&c);
      }
      timeout = next_timeout_ms(now_ns());
    }

    // Only this thread adds or removes clients, so the pointers gathered
    // above stay valid while the lock is released for the wait.
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);

    std::lock_guard<std::mutex> lock(mu_);
    const std::uint64_t now = now_ns();
    if ((fds[0].revents & POLLIN) != 0) accept_clients(now);
    for (std::size_t i = 0; i < polled.size(); ++i) {
      Client& c = *polled[i];
      if (c.dead) continue;
      const short re = fds[i + 1].revents;
      if ((re & (POLLIN | POLLHUP | POLLERR)) != 0) read_client(c, now);
    }
    service(now_ns());
    clients_.remove_if([](const Client& c) { return c.dead; });
  }
}

void ExchangeSim::accept_clients(std::uint64_t now) {
  for (;;) {
    const int fd = net::tcp_accept(listen_fd_, cfg_.tcp_nodelay);
    if (fd < 0) return;
    Client& c = clients_.emplace_back();
    c.conn = net::Conn(fd);
    c.last_rx_ns = c.last_tx_ns = now;
  }
}

void ExchangeSim::read_client(Client& c, std::uint64_t now) {
  std::array<std::uint8_t, 64 * 1024> buf;  // NOLINT(cppcoreguidelines-pro-type-member-init)
  for (int round = 0; round < 8 && !c.dead; ++round) {
    const long n = c.conn.read_some(buf);
    if (n < 0) {
      c.dead = true;
      return;
    }
    if (n == 0) return;
    c.last_rx_ns = now;
    c.parser.feed(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
    char type = 0;
    std::span<const std::uint8_t> payload;
    while (!c.dead && !c.close_after_flush) {
      const auto st = c.parser.next(type, payload);
      if (st == soup::FrameParser::Status::NeedMore) break;
      if (st == soup::FrameParser::Status::Error) {
        c.dead = true;
        return;
      }
      handle_packet(c, type, payload);
    }
    if (static_cast<std::size_t>(n) < buf.size()) return;
  }
}

void ExchangeSim::handle_packet(Client& c, char type, std::span<const std::uint8_t> payload) {
  switch (type) {
    case soup::kLoginRequest:
      if (c.session == nullptr) handle_login(c, payload);
      return;
    case soup::kUnsequencedData:
      if (c.session != nullptr) handle_ouch(c, payload);
      return;
    case soup::kLogoutRequest:
      c.dead = true;
      return;
    case soup::kClientHeartbeat:
    case soup::kDebug:
    default:
      return;
  }
}

void ExchangeSim::handle_login(Client& c, std::span<const std::uint8_t> payload) {
  soup::LoginRequest req;
  if (!soup::parse_login_request(payload, req)) {
    c.dead = true;
    return;
  }
  const auto reject = [&](char reason) {
    ++stats_.login_rejects;
    soup::append_login_rejected(c.conn.out(), reason);
    c.close_after_flush = true;
  };
  const std::string user = req.username.str();
  auto acct = accounts_.find(user);
  if (acct == accounts_.end() || acct->second != req.password.str()) {
    reject(soup::kRejectNotAuthorized);
    return;
  }
  if (!req.session.blank() && req.session.raw() != cfg_.session_id) {
    reject(soup::kRejectSessionNotAvailable);
    return;
  }
  auto& slot = sessions_[user];
  if (!slot) {
    slot = std::make_unique<Session>();
    slot->user = user;
  }
  Session& s = *slot;
  // Requested sequence 0 means "start with the next new message"; otherwise
  // replay from the requested message. Asking for the future is an error.
  if (req.seq > s.log.size() + 1) {
    reject(soup::kRejectSessionNotAvailable);
    return;
  }
  // One connection per account: a new login displaces the old connection.
  for (Client& other : clients_) {
    if (&other != &c && other.session == &s) other.dead = true;
  }
  c.session = &s;
  c.cursor = req.seq == 0 ? s.log.size() : static_cast<std::size_t>(req.seq - 1);
  ++stats_.logins;
  soup::append_login_accepted(c.conn.out(),
                              soup::LoginAccepted{soup::SessionId(cfg_.session_id), c.cursor + 1});
}

void ExchangeSim::handle_ouch(Client& c, std::span<const std::uint8_t> payload) {
  ++stats_.inbound_messages;
  Session& s = *c.session;

  bool drop_before = false;
  bool drop_after = false;
  origin_ = &s;
  cur_duplicate_ = false;
  cur_reorder_ = false;
  cur_delay_ms_ = 0;

  if (!script_.empty()) {
    ScriptedFault& f = script_.front();
    if (f.skip > 0) {
      --f.skip;
    } else {
      switch (f.action) {
        case FaultAction::DropAndDisconnect:
          drop_before = true;
          break;
        case FaultAction::ProcessThenDisconnect:
          drop_after = true;
          break;
        case FaultAction::DuplicateResponses:
          cur_duplicate_ = true;
          break;
        case FaultAction::ReorderResponses:
          cur_reorder_ = true;
          break;
        case FaultAction::DelayResponses:
          cur_delay_ms_ = f.delay_ms;
          break;
      }
      if (drop_before || drop_after) ++stats_.injected_disconnects;
      script_.pop_front();
    }
  }
  if (!drop_before && !drop_after && roll(faults_.disconnect_prob)) {
    ++stats_.injected_disconnects;
    if ((rng_() & 1) != 0) {
      drop_before = true;
    } else {
      drop_after = true;
    }
  }

  if (drop_before) {
    c.dead = true;
    origin_ = nullptr;
    return;
  }

  wire::InboundMessage m;
  if (wire::decode_inbound(payload, m) != wire::DecodeError::None) {
    ++stats_.malformed;
  } else if (const auto* e = std::get_if<wire::EnterOrder>(&m)) {
    on_enter(s, *e);
  } else if (const auto* x = std::get_if<wire::CancelOrder>(&m)) {
    on_cancel(s, *x);
  } else if (const auto* u = std::get_if<wire::ReplaceOrder>(&m)) {
    on_replace(s, *u);
  }
  origin_ = nullptr;
  if (drop_after) c.dead = true;
}

// ------------------------------------------------------------ order handling

void ExchangeSim::retire(SimOrder& o) {
  o.live = false;
  o.open = 0;
  owners_.erase(o.ref);
}

void ExchangeSim::on_enter(Session& s, const wire::EnterOrder& m) {
  // Tokens are single-use per account. A repeat is ignored without a
  // response, which is what makes a same-token resend idempotent.
  auto [it, inserted] = s.orders.try_emplace(m.token);
  if (!inserted) {
    ++stats_.duplicate_tokens;
    return;
  }
  SimOrder& o = it->second;
  o.req = m;

  char reason = wire::validate(m);
  if (reason == 0 && !symbols_.empty() && !symbols_.contains(m.stock)) reason = 'S';
  if (reason != 0) {
    ++stats_.rejected;
    emit(s, wire::Rejected{ns_since_midnight(), m.token, reason});
    return;
  }

  o.ref = next_ref_++;
  o.open = m.shares;
  o.live = true;
  owners_[o.ref] = Owner{&s, m.token};
  ++stats_.accepted;

  wire::Accepted a;
  a.timestamp = ns_since_midnight();
  a.token = m.token;
  a.side = m.side;
  a.shares = m.shares;
  a.stock = m.stock;
  a.price = m.price;
  a.tif = m.tif;
  a.firm = m.firm;
  a.display = m.display;
  a.order_ref = o.ref;
  a.capacity = m.capacity;
  a.iso = m.iso;
  a.min_qty = m.min_qty;
  a.cross_type = m.cross_type;
  a.order_state = 'L';
  emit(s, a);
  run_match(s, o);
}

void ExchangeSim::run_match(Session& s, SimOrder& o) {
  const std::uint64_t ts = ns_since_midnight();
  engine_->add(o.ref, o.req.stock, is_buy(o.req.side), o.req.price, o.open, o.req.tif == kTifIoc,
               scratch_);
  for (const Fill& f : scratch_.fills) {
    o.executed += f.shares;
    o.open -= f.shares;
    ++stats_.executions;
    emit(s, wire::Executed{ts, o.req.token, f.shares, f.price, 'R', f.match_number});

    const Owner owner = owners_.at(f.resting_ref);
    SimOrder& resting = owner.session->orders.at(owner.token);
    resting.executed += f.shares;
    resting.open -= f.shares;
    ++stats_.executions;
    emit(*owner.session, wire::Executed{ts, owner.token, f.shares, f.price, 'A', f.match_number});
    if (resting.open == 0) retire(resting);
  }
  if (scratch_.canceled > 0) {
    ++stats_.cancels;
    emit(s, wire::Canceled{ts, o.req.token, scratch_.canceled, 'I'});
    o.open -= scratch_.canceled;
  }
  if (o.open == 0) retire(o);
}

void ExchangeSim::on_cancel(Session& s, const wire::CancelOrder& m) {
  auto it = s.orders.find(m.token);
  if (it == s.orders.end() || !it->second.live) {
    ++stats_.ignored;  // nothing to cancel: silently ignored
    return;
  }
  SimOrder& o = it->second;
  const std::uint32_t removed = engine_->reduce(o.ref, m.shares);
  if (removed == 0) {
    ++stats_.ignored;
    return;
  }
  o.open -= removed;
  ++stats_.cancels;
  emit(s, wire::Canceled{ns_since_midnight(), m.token, removed, 'U'});
  if (o.open == 0) retire(o);
}

void ExchangeSim::on_replace(Session& s, const wire::ReplaceOrder& m) {
  auto it = s.orders.find(m.existing_token);
  if (it == s.orders.end() || !it->second.live) {
    ++stats_.ignored;
    return;
  }
  if (s.orders.contains(m.replacement_token)) {
    ++stats_.duplicate_tokens;
    return;
  }
  SimOrder& old = it->second;
  SimOrder& repl = s.orders[m.replacement_token];  // a rejected replace still uses its token
  repl.req = old.req;
  repl.req.token = m.replacement_token;
  repl.req.price = m.price;
  repl.req.tif = m.tif;
  repl.req.display = m.display;
  repl.req.iso = m.iso;
  repl.req.min_qty = m.min_qty;

  if (const char reason = wire::validate(m); reason != 0) {
    ++stats_.rejected;
    emit(s, wire::Rejected{ns_since_midnight(), m.replacement_token, reason});
    return;  // the existing order is untouched
  }

  const std::uint32_t removed = engine_->reduce(old.ref, 0);
  if (m.shares <= old.executed) {
    // The new total is already covered by executions: nothing left to work.
    // The existing order is canceled and, as in OUCH 4.2, the replacement
    // token is not consumed.
    ++stats_.cancels;
    emit(s, wire::Canceled{ns_since_midnight(), m.existing_token, removed, 'U'});
    retire(old);
    s.orders.erase(m.replacement_token);
    return;
  }

  const std::uint32_t open = m.shares - old.executed;
  retire(old);
  repl.req.shares = open;
  repl.ref = next_ref_++;
  repl.open = open;
  repl.live = true;
  owners_[repl.ref] = Owner{&s, m.replacement_token};
  ++stats_.replaces;

  wire::Replaced r;
  r.timestamp = ns_since_midnight();
  r.token = m.replacement_token;
  r.side = repl.req.side;
  r.shares = open;
  r.stock = repl.req.stock;
  r.price = m.price;
  r.tif = m.tif;
  r.firm = repl.req.firm;
  r.display = m.display;
  r.order_ref = repl.ref;
  r.capacity = repl.req.capacity;
  r.iso = m.iso;
  r.min_qty = m.min_qty;
  r.cross_type = repl.req.cross_type;
  r.order_state = 'L';
  r.previous_token = m.existing_token;
  emit(s, r);
  run_match(s, repl);
}

// ------------------------------------------------------- sequenced delivery

void ExchangeSim::emit(Session& s, const wire::OutboundMessage& m) {
  const std::uint64_t now = now_ns();
  LogEntry e;
  e.len = static_cast<std::uint8_t>(wire::encode(m, e.bytes));
  e.release_ns = now;

  bool duplicate = false;
  bool reorder = false;
  std::uint32_t delay_ms = 0;
  if (&s == origin_) {
    duplicate = cur_duplicate_;
    reorder = cur_reorder_;
    cur_reorder_ = false;  // only the first response is held back
    delay_ms = cur_delay_ms_;
  }
  if (faults_.any()) {
    duplicate = duplicate || roll(faults_.duplicate_prob);
    reorder = reorder || roll(faults_.reorder_prob);
    if (roll(faults_.delay_prob)) delay_ms = std::max(delay_ms, faults_.delay_ms);
  }
  if (delay_ms > 0) {
    ++stats_.injected_delays;
    e.release_ns = now + delay_ms * kNsPerMs;
  }

  if (reorder && !s.held) {
    ++stats_.injected_reorders;
    s.held = e;
    s.held_deadline_ns = now + cfg_.reorder_flush_ms * kNsPerMs;
    return;
  }
  s.log.push_back(e);
  if (duplicate) {
    ++stats_.injected_duplicates;
    s.log.push_back(e);
  }
  if (s.held) {
    s.log.push_back(*s.held);
    s.held.reset();
  }
}

void ExchangeSim::service(std::uint64_t now) {
  // Release messages held for reordering that nothing overtook in time.
  for (auto& [user, s] : sessions_) {
    if (s->held && now >= s->held_deadline_ns) {
      s->log.push_back(*s->held);
      s->held.reset();
    }
  }
  for (Client& c : clients_) {
    if (c.dead) continue;
    if (!muted_ && c.session != nullptr) {
      const std::vector<LogEntry>& log = c.session->log;
      while (c.cursor < log.size() && log[c.cursor].release_ns <= now) {
        const LogEntry& e = log[c.cursor++];
        soup::append_packet(c.conn.out(), soup::kSequencedData,
                            std::span<const std::uint8_t>(e.bytes.data(), e.len));
        c.last_tx_ns = now;
      }
      if (now - c.last_tx_ns >= cfg_.heartbeat_interval_ms * kNsPerMs) {
        soup::append_packet(c.conn.out(), soup::kServerHeartbeat);
        c.last_tx_ns = now;
      }
    }
    if (c.conn.pending() > 0 && !c.conn.flush()) {
      c.dead = true;
      continue;
    }
    if (c.close_after_flush && c.conn.pending() == 0) c.dead = true;
    if (now - c.last_rx_ns >= cfg_.client_timeout_ms * kNsPerMs) c.dead = true;
  }
}

}  // namespace ouch::sim
