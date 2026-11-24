#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mdf/protocol.hpp"
#include "mdf/socket_utils.hpp"

namespace mdf {

struct MulticastConfig {
  std::string group_ip = "239.255.0.1";
  uint16_t port = 30001;
  std::string interface_ip = "127.0.0.1";
  uint8_t ttl = 1;
  bool loopback = true;
  int rcvbuf_bytes = 2 * 1024 * 1024;
  int sndbuf_bytes = 2 * 1024 * 1024;
  bool non_blocking = false;
  int recv_timeout_ms = 50;
  int busy_poll_us = 0;
  bool use_recvmmsg = true;  // Used on Linux when available; graceful recvfrom fallback elsewhere
};

struct ReceivedDatagram {
  std::vector<uint8_t> bytes;
  uint64_t recv_ts_ns = 0;
};

class UdpMulticastSender {
 public:
  UdpMulticastSender() = default;
  ~UdpMulticastSender() = default;

  UdpMulticastSender(const UdpMulticastSender&) = delete;
  UdpMulticastSender& operator=(const UdpMulticastSender&) = delete;

  bool open(const MulticastConfig& cfg, std::string* err_out = nullptr);
  void close() noexcept { fd_.reset(-1); }
  bool is_open() const noexcept { return fd_.valid(); }

  bool send(std::span<const uint8_t> datagram, std::string* err_out = nullptr);

  int actual_sndbuf_bytes() const;
  const MulticastConfig& config() const noexcept { return cfg_; }

 private:
  ScopedFd fd_;
  MulticastConfig cfg_{};
  sockaddr_in dest_addr_{};
};

class UdpMulticastReceiver {
 public:
  UdpMulticastReceiver() = default;
  ~UdpMulticastReceiver();

  UdpMulticastReceiver(const UdpMulticastReceiver&) = delete;
  UdpMulticastReceiver& operator=(const UdpMulticastReceiver&) = delete;

  bool open(const MulticastConfig& cfg, std::string* err_out = nullptr);
  void close() noexcept;
  bool is_open() const noexcept { return fd_.valid(); }

  // Receive a single datagram. Returns:
  //   > 0: bytes received
  //   == 0: timeout / EAGAIN (no datagram available)
  //   < 0: fatal socket error
  int recv_one(std::vector<uint8_t>& out, uint64_t& recv_ts_ns);

  // Receive up to `max_batch` datagrams into `out` (appends).
  // On Linux with `use_recvmmsg = true`, uses `recvmmsg(MSG_DONTWAIT / timeout)`.
  // On macOS or when `use_recvmmsg = false`, drains with `recvfrom()`.
  std::size_t recv_batch(std::vector<ReceivedDatagram>& out, std::size_t max_batch);

  uint16_t bound_port() const noexcept { return bound_port_; }
  int actual_rcvbuf_bytes() const;
  bool busy_poll_active() const noexcept { return busy_poll_active_; }
  bool recvmmsg_supported() const noexcept;
  int fd() const noexcept { return fd_.get(); }

 private:
  ScopedFd fd_;
  MulticastConfig cfg_{};
  ip_mreq mreq_{};
  bool joined_group_ = false;
  bool busy_poll_active_ = false;
  uint16_t bound_port_ = 0;
};

}  // namespace mdf
