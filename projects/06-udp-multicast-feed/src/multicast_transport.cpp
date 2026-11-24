#include "mdf/multicast_transport.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mdf {

namespace {

std::string sys_err(const char* op) {
  return std::string(op) + ": " + std::strerror(errno);
}

}  // namespace

bool UdpMulticastSender::open(const MulticastConfig& cfg, std::string* err_out) {
  close();
  cfg_ = cfg;

  const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    if (err_out) *err_out = sys_err("socket");
    return false;
  }
  ScopedFd fd(sock);

  if (cfg.sndbuf_bytes > 0) {
    (void)set_socket_sndbuf(fd.get(), cfg.sndbuf_bytes);
  }

  in_addr iface{};
  if (::inet_pton(AF_INET, cfg.interface_ip.c_str(), &iface) != 1) {
    if (err_out) *err_out = "Invalid interface IP: " + cfg.interface_ip;
    return false;
  }
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(IP_MULTICAST_IF)");
    return false;
  }

  const unsigned char ttl = cfg.ttl;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(IP_MULTICAST_TTL)");
    return false;
  }

  const unsigned char loop = cfg.loopback ? 1U : 0U;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(IP_MULTICAST_LOOP)");
    return false;
  }

  std::memset(&dest_addr_, 0, sizeof(dest_addr_));
  dest_addr_.sin_family = AF_INET;
  dest_addr_.sin_port = htons(cfg.port);
  if (::inet_pton(AF_INET, cfg.group_ip.c_str(), &dest_addr_.sin_addr) != 1) {
    if (err_out) *err_out = "Invalid multicast group IP: " + cfg.group_ip;
    return false;
  }

  fd_ = std::move(fd);
  return true;
}

bool UdpMulticastSender::send(std::span<const uint8_t> datagram, std::string* err_out) {
  if (!fd_.valid()) {
    if (err_out) *err_out = "UdpMulticastSender not open";
    return false;
  }
  const ssize_t n = ::sendto(fd_.get(), datagram.data(), datagram.size(), 0,
                             reinterpret_cast<const sockaddr*>(&dest_addr_), sizeof(dest_addr_));
  if (n < 0 || static_cast<std::size_t>(n) != datagram.size()) {
    if (err_out) *err_out = sys_err("sendto");
    return false;
  }
  return true;
}

int UdpMulticastSender::actual_sndbuf_bytes() const {
  return fd_.valid() ? get_socket_sndbuf(fd_.get()) : -1;
}

UdpMulticastReceiver::~UdpMulticastReceiver() { close(); }

bool UdpMulticastReceiver::open(const MulticastConfig& cfg, std::string* err_out) {
  close();
  cfg_ = cfg;

  const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    if (err_out) *err_out = sys_err("socket");
    return false;
  }
  ScopedFd fd(sock);

  int reuse = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(SO_REUSEADDR)");
    return false;
  }
#ifdef SO_REUSEPORT
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(SO_REUSEPORT)");
    return false;
  }
#endif

  if (cfg.rcvbuf_bytes > 0) {
    (void)set_socket_rcvbuf(fd.get(), cfg.rcvbuf_bytes);
  }

  sockaddr_in local_addr{};
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(cfg.port);
  local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&local_addr), sizeof(local_addr)) != 0) {
    if (err_out) *err_out = sys_err("bind");
    return false;
  }

  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = cfg.port;
  }

  std::memset(&mreq_, 0, sizeof(mreq_));
  if (::inet_pton(AF_INET, cfg.group_ip.c_str(), &mreq_.imr_multiaddr) != 1) {
    if (err_out) *err_out = "Invalid multicast group IP: " + cfg.group_ip;
    return false;
  }
  if (::inet_pton(AF_INET, cfg.interface_ip.c_str(), &mreq_.imr_interface) != 1) {
    if (err_out) *err_out = "Invalid interface IP: " + cfg.interface_ip;
    return false;
  }
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq_, sizeof(mreq_)) != 0) {
    if (err_out) *err_out = sys_err("setsockopt(IP_ADD_MEMBERSHIP)");
    return false;
  }
  joined_group_ = true;

  if (cfg.non_blocking) {
    if (!set_nonblocking(fd.get(), true)) {
      if (err_out) *err_out = sys_err("fcntl(O_NONBLOCK)");
      return false;
    }
  } else if (cfg.recv_timeout_ms > 0) {
    (void)set_recv_timeout_ms(fd.get(), cfg.recv_timeout_ms);
  }

  busy_poll_active_ = false;
  if (cfg.busy_poll_us > 0) {
    busy_poll_active_ = set_busy_poll_us(fd.get(), cfg.busy_poll_us, nullptr);
  }

  fd_ = std::move(fd);
  return true;
}

void UdpMulticastReceiver::close() noexcept {
  if (fd_.valid() && joined_group_) {
    (void)::setsockopt(fd_.get(), IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq_, sizeof(mreq_));
  }
  joined_group_ = false;
  busy_poll_active_ = false;
  bound_port_ = 0;
  fd_.reset(-1);
}

int UdpMulticastReceiver::recv_one(std::vector<uint8_t>& out, uint64_t& recv_ts_ns) {
  if (!fd_.valid()) return -1;
  out.resize(kMaxUdpDatagramSize);
  const ssize_t n = ::recvfrom(fd_.get(), out.data(), out.size(), 0, nullptr, nullptr);
  if (n < 0) {
    out.clear();
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return 0;
    }
    return -1;
  }
  recv_ts_ns = steady_now_ns();
  out.resize(static_cast<std::size_t>(n));
  return static_cast<int>(n);
}

bool UdpMulticastReceiver::recvmmsg_supported() const noexcept {
#ifdef __linux__
  return cfg_.use_recvmmsg;
#else
  return false;
#endif
}

std::size_t UdpMulticastReceiver::recv_batch(std::vector<ReceivedDatagram>& out,
                                             std::size_t max_batch) {
  if (!fd_.valid() || max_batch == 0) return 0;

#ifdef __linux__
  if (cfg_.use_recvmmsg && max_batch > 1) {
    std::vector<std::vector<uint8_t>> buffers(max_batch,
                                              std::vector<uint8_t>(kMaxUdpDatagramSize));
    std::vector<iovec> iovs(max_batch);
    std::vector<mmsghdr> msgs(max_batch);
    std::memset(msgs.data(), 0, msgs.size() * sizeof(mmsghdr));
    for (std::size_t i = 0; i < max_batch; ++i) {
      iovs[i].iov_base = buffers[i].data();
      iovs[i].iov_len = buffers[i].size();
      msgs[i].msg_hdr.msg_iov = &iovs[i];
      msgs[i].msg_hdr.msg_iovlen = 1;
    }

    int flags = cfg_.non_blocking ? MSG_DONTWAIT : MSG_WAITFORONE;
    const int n = ::recvmmsg(fd_.get(), msgs.data(), static_cast<unsigned int>(max_batch), flags,
                             nullptr);
    if (n <= 0) return 0;
    const uint64_t ts = steady_now_ns();
    for (int i = 0; i < n; ++i) {
      const auto idx = static_cast<std::size_t>(i);
      buffers[idx].resize(msgs[idx].msg_len);
      out.push_back(ReceivedDatagram{std::move(buffers[idx]), ts});
    }
    return static_cast<std::size_t>(n);
  }
#endif

  // Portable path: read first datagram (respecting blocking/timeout or non-blocking),
  // then drain up to max_batch - 1 already-queued datagrams with MSG_DONTWAIT.
  std::size_t count = 0;
  std::vector<uint8_t> buf(kMaxUdpDatagramSize);
  const ssize_t first = ::recvfrom(fd_.get(), buf.data(), buf.size(), 0, nullptr, nullptr);
  if (first <= 0) return 0;
  buf.resize(static_cast<std::size_t>(first));
  out.push_back(ReceivedDatagram{std::move(buf), steady_now_ns()});
  ++count;

  while (count < max_batch) {
    std::vector<uint8_t> next_buf(kMaxUdpDatagramSize);
    const ssize_t n =
        ::recvfrom(fd_.get(), next_buf.data(), next_buf.size(), MSG_DONTWAIT, nullptr, nullptr);
    if (n <= 0) break;
    next_buf.resize(static_cast<std::size_t>(n));
    out.push_back(ReceivedDatagram{std::move(next_buf), steady_now_ns()});
    ++count;
  }
  return count;
}

int UdpMulticastReceiver::actual_rcvbuf_bytes() const {
  return fd_.valid() ? get_socket_rcvbuf(fd_.get()) : -1;
}

}  // namespace mdf
