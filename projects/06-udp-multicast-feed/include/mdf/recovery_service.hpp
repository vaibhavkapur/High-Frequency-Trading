#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "mdf/order_book.hpp"
#include "mdf/protocol.hpp"
#include "mdf/socket_utils.hpp"

namespace mdf {

struct RecoveryStoreConfig {
  std::size_t retransmit_buffer_packets = 1024;
  std::size_t max_retransmit_range = 256;
  // Take a consistent replay checkpoint every N packets (0 = only explicit checkpoints).
  uint64_t checkpoint_interval = 200;
  // Guarantee the latest replay checkpoint never falls behind the retransmit ring buffer
  // tail by capturing an automatic checkpoint once the gap reaches half the ring capacity.
  bool auto_checkpoint_before_eviction = true;
};

struct RecoveryStoreStats {
  uint64_t packets_recorded = 0;
  uint64_t packets_evicted = 0;
  uint64_t checkpoints_created = 0;
  uint64_t retransmit_requests = 0;
  uint64_t retransmit_ok = 0;
  uint64_t retransmit_unavailable = 0;
  uint64_t retransmit_packets_sent = 0;
  uint64_t snapshot_requests = 0;
  uint64_t snapshots_sent = 0;
};

// Thread-safe storage for bounded per-channel retransmission buffers and replay checkpoints.
class RecoveryStore {
 public:
  explicit RecoveryStore(RecoveryStoreConfig cfg = {});

  void register_channel(uint16_t channel_id);

  // Record a newly published packet (must be strictly increasing seq_num per channel)
  // alongside the authoritative channel state after applying that packet.
  void record_packet(uint16_t channel_id,
                     uint64_t seq_num,
                     std::span<const uint8_t> raw_packet,
                     const ChannelMarketState& authoritative_state);

  // Force creation of a replay checkpoint at authoritative_state.last_applied_seq().
  void create_checkpoint(uint16_t channel_id, const ChannelMarketState& authoritative_state);

  RetransmitResponse handle_retransmit(const RetransmitRequest& req);
  SnapshotResponse handle_snapshot(const SnapshotRequest& req);

  RecoveryStoreStats stats() const;
  uint64_t oldest_available_seq(uint16_t channel_id) const;
  uint64_t latest_seq(uint16_t channel_id) const;
  uint64_t latest_checkpoint_seq(uint16_t channel_id) const;

 private:
  struct BufferedPacket {
    uint64_t seq_num = 0;
    std::vector<uint8_t> raw_bytes;
  };

  struct ChannelHistory {
    uint64_t latest_seq = 0;
    uint64_t next_checkpoint_id = 1;
    std::deque<BufferedPacket> ring;
    std::optional<SnapshotData> latest_checkpoint;
  };

  RecoveryStoreConfig cfg_{};
  mutable std::mutex mu_;
  std::map<uint16_t, ChannelHistory> channels_;
  RecoveryStoreStats stats_{};
};

// Abstract client interface used by ChannelReceiver to request retransmission or snapshots.
class IRecoveryClient {
 public:
  virtual ~IRecoveryClient() = default;
  virtual bool request_retransmit(const RetransmitRequest& req, RetransmitResponse& out) = 0;
  virtual bool request_snapshot(const SnapshotRequest& req, SnapshotResponse& out) = 0;
};

// In-memory recovery client for deterministic unit/property tests (zero socket overhead).
class InMemoryRecoveryClient final : public IRecoveryClient {
 public:
  explicit InMemoryRecoveryClient(RecoveryStore& store) : store_(store) {}

  bool request_retransmit(const RetransmitRequest& req, RetransmitResponse& out) override;
  bool request_snapshot(const SnapshotRequest& req, SnapshotResponse& out) override;

  void set_available(bool available) noexcept { available_ = available; }

 private:
  RecoveryStore& store_;
  bool available_ = true;
};

struct TcpRecoveryServerConfig {
  std::string bind_ip = "127.0.0.1";
  uint16_t port = 0;  // 0 = OS-assigned ephemeral port
  int io_timeout_ms = 5000;
};

// Multi-client TCP server serving MDR1 retransmission and snapshot requests from a RecoveryStore.
class TcpRecoveryServer {
 public:
  explicit TcpRecoveryServer(RecoveryStore& store);
  ~TcpRecoveryServer();

  TcpRecoveryServer(const TcpRecoveryServer&) = delete;
  TcpRecoveryServer& operator=(const TcpRecoveryServer&) = delete;

  bool start(const TcpRecoveryServerConfig& cfg, std::string* err_out = nullptr);
  void stop();

  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  uint16_t bound_port() const noexcept { return bound_port_; }
  uint64_t connections_accepted() const noexcept {
    return connections_accepted_.load(std::memory_order_relaxed);
  }

 private:
  void accept_loop();
  void handle_client(ScopedFd client_fd);

  RecoveryStore& store_;
  TcpRecoveryServerConfig cfg_{};
  ScopedFd listen_fd_;
  uint16_t bound_port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> connections_accepted_{0};
  std::thread accept_thread_;
  std::mutex workers_mu_;
  std::vector<std::thread> workers_;
};

// Persistent TCP client for MDR1 retransmission and snapshot requests.
class TcpRecoveryClient final : public IRecoveryClient {
 public:
  TcpRecoveryClient(std::string host, uint16_t port, int timeout_ms = 5000);
  ~TcpRecoveryClient() override;

  TcpRecoveryClient(const TcpRecoveryClient&) = delete;
  TcpRecoveryClient& operator=(const TcpRecoveryClient&) = delete;

  bool request_retransmit(const RetransmitRequest& req, RetransmitResponse& out) override;
  bool request_snapshot(const SnapshotRequest& req, SnapshotResponse& out) override;

  void disconnect() noexcept { fd_.reset(-1); }

 private:
  bool ensure_connected();
  bool round_trip(std::span<const uint8_t> req_frame,
                  RecoveryMsgType expected_resp_type,
                  std::vector<uint8_t>& resp_payload);

  std::string host_;
  uint16_t port_ = 0;
  int timeout_ms_ = 5000;
  ScopedFd fd_;
  std::mutex mu_;
};

}  // namespace mdf
