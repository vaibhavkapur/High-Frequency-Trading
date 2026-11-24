#include "mdf/recovery_service.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mdf {

RecoveryStore::RecoveryStore(RecoveryStoreConfig cfg) : cfg_(cfg) {}

void RecoveryStore::register_channel(uint16_t channel_id) {
  std::lock_guard<std::mutex> lock(mu_);
  (void)channels_[channel_id];
}

void RecoveryStore::record_packet(uint16_t channel_id,
                                  uint64_t seq_num,
                                  std::span<const uint8_t> raw_packet,
                                  const ChannelMarketState& authoritative_state) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& ch = channels_[channel_id];
  ch.latest_seq = seq_num;

  if (cfg_.retransmit_buffer_packets > 0) {
    if (ch.ring.size() >= cfg_.retransmit_buffer_packets) {
      ch.ring.pop_front();
      ++stats_.packets_evicted;
    }
    ch.ring.push_back(BufferedPacket{
        seq_num, std::vector<uint8_t>(raw_packet.begin(), raw_packet.end())});
  }
  ++stats_.packets_recorded;

  const bool near_eviction =
      cfg_.auto_checkpoint_before_eviction && ch.latest_checkpoint.has_value() &&
      cfg_.retransmit_buffer_packets > 1 && seq_num >= ch.latest_checkpoint->as_of_seq &&
      (seq_num - ch.latest_checkpoint->as_of_seq) >= (cfg_.retransmit_buffer_packets / 2);

  const bool should_checkpoint =
      !ch.latest_checkpoint.has_value() || near_eviction ||
      (cfg_.checkpoint_interval > 0 && (seq_num % cfg_.checkpoint_interval == 0));
  if (should_checkpoint) {
    ch.latest_checkpoint = authoritative_state.export_snapshot(ch.next_checkpoint_id++);
    ++stats_.checkpoints_created;
  }
}

void RecoveryStore::create_checkpoint(uint16_t channel_id,
                                      const ChannelMarketState& authoritative_state) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& ch = channels_[channel_id];
  ch.latest_checkpoint = authoritative_state.export_snapshot(ch.next_checkpoint_id++);
  ++stats_.checkpoints_created;
}

RetransmitResponse RecoveryStore::handle_retransmit(const RetransmitRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.retransmit_requests;

  RetransmitResponse resp{};
  resp.channel_id = req.channel_id;

  auto it = channels_.find(req.channel_id);
  if (it == channels_.end()) {
    resp.status = RecoveryStatus::kUnknownChannel;
    ++stats_.retransmit_unavailable;
    return resp;
  }

  const auto& ch = it->second;
  resp.latest_seq = ch.latest_seq;
  resp.oldest_available_seq = ch.ring.empty() ? 0 : ch.ring.front().seq_num;
  resp.latest_snapshot_seq =
      ch.latest_checkpoint.has_value() ? ch.latest_checkpoint->as_of_seq : 0;

  if (req.from_seq == 0 || req.to_seq < req.from_seq) {
    resp.status = RecoveryStatus::kInvalidRequest;
    ++stats_.retransmit_unavailable;
    return resp;
  }

  const uint64_t count = req.to_seq - req.from_seq + 1;
  if (count > cfg_.max_retransmit_range || ch.ring.empty() ||
      req.from_seq < ch.ring.front().seq_num || req.to_seq > ch.ring.back().seq_num) {
    resp.status = RecoveryStatus::kUnavailable;
    ++stats_.retransmit_unavailable;
    return resp;
  }

  // Contiguous sequences in ring buffer allow O(1) index offset.
  const uint64_t base_seq = ch.ring.front().seq_num;
  const std::size_t start_idx = static_cast<std::size_t>(req.from_seq - base_seq);
  const std::size_t end_idx = static_cast<std::size_t>(req.to_seq - base_seq);
  if (end_idx >= ch.ring.size() || ch.ring[start_idx].seq_num != req.from_seq ||
      ch.ring[end_idx].seq_num != req.to_seq) {
    resp.status = RecoveryStatus::kUnavailable;
    ++stats_.retransmit_unavailable;
    return resp;
  }

  resp.status = RecoveryStatus::kOk;
  resp.raw_packets.reserve(static_cast<std::size_t>(count));
  for (std::size_t i = start_idx; i <= end_idx; ++i) {
    resp.raw_packets.push_back(ch.ring[i].raw_bytes);
  }
  ++stats_.retransmit_ok;
  stats_.retransmit_packets_sent += resp.raw_packets.size();
  return resp;
}

SnapshotResponse RecoveryStore::handle_snapshot(const SnapshotRequest& req) {
  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.snapshot_requests;

  SnapshotResponse resp{};
  resp.snapshot.channel_id = req.channel_id;

  auto it = channels_.find(req.channel_id);
  if (it == channels_.end()) {
    resp.status = RecoveryStatus::kUnknownChannel;
    return resp;
  }
  const auto& ch = it->second;
  if (!ch.latest_checkpoint.has_value()) {
    resp.status = RecoveryStatus::kUnavailable;
    return resp;
  }
  resp.status = RecoveryStatus::kOk;
  resp.snapshot = *ch.latest_checkpoint;
  ++stats_.snapshots_sent;
  return resp;
}

RecoveryStoreStats RecoveryStore::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

uint64_t RecoveryStore::oldest_available_seq(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = channels_.find(channel_id);
  if (it == channels_.end() || it->second.ring.empty()) return 0;
  return it->second.ring.front().seq_num;
}

uint64_t RecoveryStore::latest_seq(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) return 0;
  return it->second.latest_seq;
}

uint64_t RecoveryStore::latest_checkpoint_seq(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = channels_.find(channel_id);
  if (it == channels_.end() || !it->second.latest_checkpoint.has_value()) return 0;
  return it->second.latest_checkpoint->as_of_seq;
}

bool InMemoryRecoveryClient::request_retransmit(const RetransmitRequest& req,
                                                RetransmitResponse& out) {
  if (!available_) return false;
  out = store_.handle_retransmit(req);
  return true;
}

bool InMemoryRecoveryClient::request_snapshot(const SnapshotRequest& req, SnapshotResponse& out) {
  if (!available_) return false;
  out = store_.handle_snapshot(req);
  return true;
}

TcpRecoveryServer::TcpRecoveryServer(RecoveryStore& store) : store_(store) {}

TcpRecoveryServer::~TcpRecoveryServer() { stop(); }

bool TcpRecoveryServer::start(const TcpRecoveryServerConfig& cfg, std::string* err_out) {
  stop();
  cfg_ = cfg;

  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    if (err_out) *err_out = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  ScopedFd fd(sock);

  int reuse = 1;
  (void)::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  (void)disable_sigpipe(fd.get());

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg.port);
  if (::inet_pton(AF_INET, cfg.bind_ip.c_str(), &addr.sin_addr) != 1) {
    if (err_out) *err_out = "Invalid bind IP: " + cfg.bind_ip;
    return false;
  }
  if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err_out) *err_out = std::string("bind: ") + std::strerror(errno);
    return false;
  }
  if (::listen(fd.get(), 64) != 0) {
    if (err_out) *err_out = std::string("listen: ") + std::strerror(errno);
    return false;
  }

  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = cfg.port;
  }

  listen_fd_ = std::move(fd);
  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return true;
}

void TcpRecoveryServer::stop() {
  running_.store(false, std::memory_order_release);
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  listen_fd_.reset(-1);

  std::vector<std::thread> to_join;
  {
    std::lock_guard<std::mutex> lock(workers_mu_);
    to_join.swap(workers_);
  }
  for (auto& t : to_join) {
    if (t.joinable()) t.join();
  }
  bound_port_ = 0;
}

void TcpRecoveryServer::accept_loop() {
  while (running_.load(std::memory_order_acquire)) {
    struct pollfd pfd{};
    pfd.fd = listen_fd_.get();
    pfd.events = POLLIN;
    const int prc = ::poll(&pfd, 1, 25);
    if (prc <= 0) continue;
    if ((pfd.revents & POLLIN) == 0) continue;

    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const int client = ::accept(listen_fd_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (client < 0) continue;

    int nodelay = 1;
    (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    (void)disable_sigpipe(client);

    connections_accepted_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(workers_mu_);
    workers_.emplace_back([this, client]() { handle_client(ScopedFd(client)); });
  }
}

void TcpRecoveryServer::handle_client(ScopedFd client_fd) {
  while (running_.load(std::memory_order_acquire)) {
    // Poll with short timeout so stop() wakes promptly even when clients stay idle.
    struct pollfd pfd{};
    pfd.fd = client_fd.get();
    pfd.events = POLLIN;
    const int prc = ::poll(&pfd, 1, 25);
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (prc == 0) continue;
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) break;

    uint8_t hdr_buf[kRecoveryFrameHeaderSize];
    if (!recv_exact(client_fd.get(), hdr_buf, sizeof(hdr_buf), cfg_.io_timeout_ms)) {
      break;
    }

    RecoveryFrameHeader hdr{};
    if (decode_recovery_frame_header(hdr_buf, hdr) != DecodeStatus::kOk) {
      break;
    }

    std::vector<uint8_t> payload(hdr.payload_len);
    if (hdr.payload_len > 0 &&
        !recv_exact(client_fd.get(), payload.data(), payload.size(), cfg_.io_timeout_ms)) {
      break;
    }

    std::vector<uint8_t> resp_frame;
    if (hdr.type == RecoveryMsgType::kRetransmitReq) {
      RetransmitRequest req{};
      if (decode_retransmit_request(payload, req) != DecodeStatus::kOk) break;
      const RetransmitResponse resp = store_.handle_retransmit(req);
      if (!encode_retransmit_response(resp, resp_frame)) break;
    } else if (hdr.type == RecoveryMsgType::kSnapshotReq) {
      SnapshotRequest req{};
      if (decode_snapshot_request(payload, req) != DecodeStatus::kOk) break;
      const SnapshotResponse resp = store_.handle_snapshot(req);
      if (!encode_snapshot_response(resp, resp_frame)) break;
    } else {
      break;
    }

    if (!send_all(client_fd.get(), resp_frame, cfg_.io_timeout_ms)) {
      break;
    }
  }
}

TcpRecoveryClient::TcpRecoveryClient(std::string host, uint16_t port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

TcpRecoveryClient::~TcpRecoveryClient() { disconnect(); }

bool TcpRecoveryClient::ensure_connected() {
  if (fd_.valid()) return true;

  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;
  ScopedFd fd(sock);

  int nodelay = 1;
  (void)::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  (void)disable_sigpipe(fd.get());
  (void)set_recv_timeout_ms(fd.get(), timeout_ms_);
  (void)set_send_timeout_ms(fd.get(), timeout_ms_);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    return false;
  }
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return false;
  }

  fd_ = std::move(fd);
  return true;
}

bool TcpRecoveryClient::round_trip(std::span<const uint8_t> req_frame,
                                   RecoveryMsgType expected_resp_type,
                                   std::vector<uint8_t>& resp_payload) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensure_connected()) return false;

    if (!send_all(fd_.get(), req_frame, timeout_ms_)) {
      fd_.reset(-1);
      continue;
    }

    uint8_t hdr_buf[kRecoveryFrameHeaderSize];
    if (!recv_exact(fd_.get(), hdr_buf, sizeof(hdr_buf), timeout_ms_)) {
      fd_.reset(-1);
      continue;
    }

    RecoveryFrameHeader hdr{};
    if (decode_recovery_frame_header(hdr_buf, hdr) != DecodeStatus::kOk ||
        hdr.type != expected_resp_type) {
      fd_.reset(-1);
      return false;
    }

    resp_payload.resize(hdr.payload_len);
    if (hdr.payload_len > 0 &&
        !recv_exact(fd_.get(), resp_payload.data(), resp_payload.size(), timeout_ms_)) {
      fd_.reset(-1);
      continue;
    }
    return true;
  }
  return false;
}

bool TcpRecoveryClient::request_retransmit(const RetransmitRequest& req, RetransmitResponse& out) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<uint8_t> req_frame;
  if (!encode_retransmit_request(req, req_frame)) return false;

  std::vector<uint8_t> resp_payload;
  if (!round_trip(req_frame, RecoveryMsgType::kRetransmitResp, resp_payload)) {
    return false;
  }
  return decode_retransmit_response(resp_payload, out) == DecodeStatus::kOk;
}

bool TcpRecoveryClient::request_snapshot(const SnapshotRequest& req, SnapshotResponse& out) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<uint8_t> req_frame;
  if (!encode_snapshot_request(req, req_frame)) return false;

  std::vector<uint8_t> resp_payload;
  if (!round_trip(req_frame, RecoveryMsgType::kSnapshotResp, resp_payload)) {
    return false;
  }
  return decode_snapshot_response(resp_payload, out) == DecodeStatus::kOk;
}

}  // namespace mdf
