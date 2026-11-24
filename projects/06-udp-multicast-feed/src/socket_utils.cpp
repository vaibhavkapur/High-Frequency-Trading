#include "mdf/socket_utils.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace mdf {

ScopedFd::~ScopedFd() { reset(-1); }

ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
  if (this != &other) {
    reset(other.fd_);
    other.fd_ = -1;
  }
  return *this;
}

void ScopedFd::reset(int new_fd) noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = new_fd;
}

int ScopedFd::release() noexcept {
  const int old = fd_;
  fd_ = -1;
  return old;
}

bool set_socket_rcvbuf(int fd, int bytes) {
  if (bytes <= 0) return true;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

bool set_socket_sndbuf(int fd, int bytes) {
  if (bytes <= 0) return true;
  return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
}

int get_socket_rcvbuf(int fd) {
  int val = 0;
  socklen_t len = sizeof(val);
  if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, &len) != 0) return -1;
  return val;
}

int get_socket_sndbuf(int fd) {
  int val = 0;
  socklen_t len = sizeof(val);
  if (::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &len) != 0) return -1;
  return val;
}

bool set_nonblocking(int fd, bool non_blocking) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  const int next = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, next) == 0;
}

bool set_recv_timeout_ms(int fd, int timeout_ms) {
  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool set_send_timeout_ms(int fd, int timeout_ms) {
  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool disable_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int on = 1;
  return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
  (void)fd;
  return true;
#endif
}

bool set_busy_poll_us(int fd, int busy_poll_us, std::string* err_out) {
  if (busy_poll_us <= 0) return true;
#if defined(__linux__) && defined(SO_BUSY_POLL)
  if (::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us)) == 0) {
    return true;
  }
  if (err_out != nullptr) {
    *err_out = std::string("SO_BUSY_POLL failed: ") + std::strerror(errno);
  }
  return false;
#else
  (void)fd;
  if (err_out != nullptr) {
    *err_out = "SO_BUSY_POLL is only supported on Linux";
  }
  return false;
#endif
}

bool pin_current_thread_to_core(int core_id, std::string* err_out) {
  if (core_id < 0) return false;
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc == 0) return true;
  if (err_out != nullptr) {
    *err_out = std::string("pthread_setaffinity_np failed: ") + std::strerror(rc);
  }
  return false;
#else
  if (err_out != nullptr) {
    *err_out = "CPU core affinity pinning via pthread_setaffinity_np is Linux-only";
  }
  return false;
#endif
}

std::optional<uint64_t> read_kernel_udp_rcvbuf_errors() {
#ifdef __linux__
  std::ifstream in("/proc/net/snmp");
  if (!in.is_open()) return std::nullopt;
  std::string header_line;
  std::string values_line;
  while (std::getline(in, header_line)) {
    if (header_line.rfind("Udp:", 0) == 0) {
      if (!std::getline(in, values_line)) return std::nullopt;
      std::istringstream hs(header_line);
      std::istringstream vs(values_line);
      std::string col;
      std::string val;
      while (hs >> col && vs >> val) {
        if (col == "RcvbufErrors") {
          return std::stoull(val);
        }
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
#else
  return std::nullopt;
#endif
}

bool send_all(int fd, std::span<const uint8_t> data, int timeout_ms) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int prc = ::poll(&pfd, 1, timeout_ms);
    if (prc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (prc == 0) return false;

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, flags);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool recv_exact(int fd, uint8_t* dst, std::size_t len, int timeout_ms) {
  std::size_t received = 0;
  while (received < len) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = ::poll(&pfd, 1, timeout_ms);
    if (prc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (prc == 0) return false;
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) return false;

    const ssize_t n = ::recv(fd, dst + received, len - received, 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    if (n == 0) return false;  // Peer closed cleanly before full frame.
    received += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace mdf
