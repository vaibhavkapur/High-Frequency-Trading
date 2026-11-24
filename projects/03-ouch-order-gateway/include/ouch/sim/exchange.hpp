// Local exchange simulator: SoupBinTCP server + OUCH order handling + matching
// core + controlled fault injection. It never connects anywhere; it only
// listens on the loopback interface.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ouch/sim/matching_engine.hpp"
#include "ouch/socket.hpp"
#include "ouch/soupbin.hpp"
#include "ouch/types.hpp"
#include "ouch/wire.hpp"

namespace ouch::sim {

// Random faults, drawn from a seeded generator so a run can be repeated.
struct FaultConfig {
  // Per inbound OUCH message: drop the client's connection. Half the time the
  // message is discarded unprocessed, half the time it is processed and the
  // responses are left for the client to recover by sequenced replay.
  double disconnect_prob = 0.0;
  // Per sequenced message: emit it twice.
  double duplicate_prob = 0.0;
  // Per sequenced message: hold it back and emit it after the next message
  // for the same session (or after reorder_flush_ms if none follows).
  double reorder_prob = 0.0;
  // Per sequenced message: delay delivery (and everything queued behind it).
  double delay_prob = 0.0;
  std::uint32_t delay_ms = 2;
  std::uint64_t seed = 1;

  static FaultConfig uniform(double rate, std::uint64_t seed = 1) {
    FaultConfig f;
    f.disconnect_prob = f.duplicate_prob = f.reorder_prob = f.delay_prob = rate;
    f.seed = seed;
    return f;
  }
  [[nodiscard]] bool any() const {
    return disconnect_prob > 0 || duplicate_prob > 0 || reorder_prob > 0 || delay_prob > 0;
  }
};

// Deterministic one-shot faults for tests: each applies to a single inbound
// OUCH message, after letting `skip` messages through untouched.
enum class FaultAction : std::uint8_t {
  DropAndDisconnect,      // discard the message, then drop the connection
  ProcessThenDisconnect,  // process it, withhold the responses, drop the connection
  DuplicateResponses,     // emit every response to the sender twice
  ReorderResponses,       // hold the first response until after the next one
  DelayResponses,         // delay the responses by delay_ms
};

struct ScriptedFault {
  FaultAction action = FaultAction::DropAndDisconnect;
  std::uint32_t skip = 0;
  std::uint32_t delay_ms = 0;
};

struct SimConfig {
  std::uint16_t port = 0;  // 0 = pick an ephemeral port
  std::vector<std::pair<std::string, std::string>> accounts = {{"user", "pass"}};
  std::vector<std::string> symbols;  // empty = accept any symbol
  std::string session_id = "SIM0000001";
  std::uint32_t heartbeat_interval_ms = 1'000;
  std::uint32_t client_timeout_ms = 15'000;
  std::uint32_t reorder_flush_ms = 5;
  bool tcp_nodelay = true;
  FaultConfig faults;
};

// Exchange-side ground truth for one order token.
struct OrderTruth {
  Token token;
  std::uint32_t open = 0;
  std::uint32_t executed = 0;
  bool live = false;
};

struct SimStats {
  std::uint64_t inbound_messages = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t executions = 0;  // Executed messages emitted (two per trade)
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t duplicate_tokens = 0;  // requests ignored because the token was already used
  std::uint64_t ignored = 0;           // cancels/replaces for unknown or dead orders
  std::uint64_t malformed = 0;
  std::uint64_t logins = 0;
  std::uint64_t login_rejects = 0;
  std::uint64_t injected_disconnects = 0;
  std::uint64_t injected_duplicates = 0;
  std::uint64_t injected_reorders = 0;
  std::uint64_t injected_delays = 0;
};

class ExchangeSim {
 public:
  explicit ExchangeSim(SimConfig cfg = {}, std::unique_ptr<MatchingEngine> engine = nullptr);
  ~ExchangeSim();
  ExchangeSim(const ExchangeSim&) = delete;
  ExchangeSim& operator=(const ExchangeSim&) = delete;

  // Binds, listens and starts the event-loop thread. False if the port is taken.
  bool start();
  void stop();
  [[nodiscard]] std::uint16_t port() const { return port_; }

  // --- controls; safe to call from any thread while the simulator runs.
  void set_faults(const FaultConfig& f);
  void inject(const ScriptedFault& f);
  void kick_all();             // drop every client connection
  void set_muted(bool muted);  // when muted the simulator sends nothing at all

  // --- observation.
  [[nodiscard]] SimStats stats() const;
  [[nodiscard]] std::vector<OrderTruth> truth(const std::string& user) const;
  // Sequenced messages produced so far for `user`, including one still held
  // back by reorder injection.
  [[nodiscard]] std::uint64_t sequenced_count(const std::string& user) const;

 private:
  struct LogEntry {
    std::uint64_t release_ns = 0;
    std::uint8_t len = 0;
    std::array<std::uint8_t, wire::kMaxMessageSize> bytes{};
  };
  struct SimOrder {
    std::uint64_t ref = 0;
    wire::EnterOrder req;
    std::uint32_t open = 0;
    std::uint32_t executed = 0;
    bool live = false;
  };
  struct Session {
    std::string user;
    std::vector<LogEntry> log;  // sequenced messages; sequence number = index + 1
    std::unordered_map<Token, SimOrder, TokenHash> orders;
    std::optional<LogEntry> held;
    std::uint64_t held_deadline_ns = 0;
  };
  struct Client {
    net::Conn conn;
    soup::FrameParser parser;
    Session* session = nullptr;
    std::size_t cursor = 0;  // next log index to deliver
    std::uint64_t last_rx_ns = 0;
    std::uint64_t last_tx_ns = 0;
    bool close_after_flush = false;
    bool dead = false;
  };
  struct Owner {
    Session* session;
    Token token;
  };

  void run();
  int next_timeout_ms(std::uint64_t now) const;
  void accept_clients(std::uint64_t now);
  void read_client(Client& c, std::uint64_t now);
  void handle_packet(Client& c, char type, std::span<const std::uint8_t> payload);
  void handle_login(Client& c, std::span<const std::uint8_t> payload);
  void handle_ouch(Client& c, std::span<const std::uint8_t> payload);
  void on_enter(Session& s, const wire::EnterOrder& m);
  void on_cancel(Session& s, const wire::CancelOrder& m);
  void on_replace(Session& s, const wire::ReplaceOrder& m);
  void run_match(Session& s, SimOrder& o);
  void retire(SimOrder& o);
  void emit(Session& s, const wire::OutboundMessage& m);
  void service(std::uint64_t now);
  bool roll(double p);

  SimConfig cfg_;
  std::unique_ptr<MatchingEngine> engine_;
  std::unordered_set<Symbol, SymbolHash> symbols_;
  std::unordered_map<std::string, std::string> accounts_;

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mu_;  // guards everything below
  std::list<Client> clients_;
  std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
  std::unordered_map<std::uint64_t, Owner> owners_;  // live order ref -> owner
  std::uint64_t next_ref_ = 1;
  AddResult scratch_;
  SimStats stats_;
  bool muted_ = false;

  FaultConfig faults_;
  std::mt19937_64 rng_;
  std::list<ScriptedFault> script_;
  // Scripted faults in force while one inbound message is being processed.
  Session* origin_ = nullptr;
  bool cur_duplicate_ = false;
  bool cur_reorder_ = false;
  std::uint32_t cur_delay_ms_ = 0;
};

}  // namespace ouch::sim
