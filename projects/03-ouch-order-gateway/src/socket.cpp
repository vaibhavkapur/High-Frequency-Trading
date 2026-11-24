#include "ouch/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace ouch::net {
namespace {

#ifdef MSG_NOSIGNAL
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

void configure(int fd, bool nodelay) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int one = 1;
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  if (nodelay) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

}  // namespace

int tcp_listen(std::uint16_t port, std::uint16_t& bound_port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 64) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  bound_port = ntohs(addr.sin_port);
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return fd;
}

int tcp_connect(const std::string& host, std::uint16_t port, bool nodelay) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return -1;
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int rc;
  do {
    rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    ::close(fd);
    return -1;
  }
  configure(fd, nodelay);
  return fd;
}

int tcp_accept(int listen_fd, bool nodelay) {
  int fd = ::accept(listen_fd, nullptr, nullptr);
  if (fd < 0) return -1;
  configure(fd, nodelay);
  return fd;
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

Conn::Conn(Conn&& o) noexcept
    : fd_(std::exchange(o.fd_, -1)),
      out_(std::move(o.out_)),
      out_off_(std::exchange(o.out_off_, 0)) {}

Conn& Conn::operator=(Conn&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = std::exchange(o.fd_, -1);
    out_ = std::move(o.out_);
    out_off_ = std::exchange(o.out_off_, 0);
  }
  return *this;
}

void Conn::close() {
  close_fd(fd_);
  out_.clear();
  out_off_ = 0;
}

bool Conn::flush() {
  if (fd_ < 0) return false;
  while (out_off_ < out_.size()) {
    const ssize_t n = ::send(fd_, out_.data() + out_off_, out_.size() - out_off_, kSendFlags);
    if (n > 0) {
      out_off_ += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      return false;
    }
  }
  if (out_off_ == out_.size()) {
    out_.clear();
    out_off_ = 0;
  }
  return true;
}

long Conn::read_some(std::span<std::uint8_t> buf) {
  if (fd_ < 0) return -1;
  for (;;) {
    const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
    if (n > 0) return static_cast<long>(n);
    if (n == 0) return -1;  // orderly shutdown by the peer
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
}

}  // namespace ouch::net
