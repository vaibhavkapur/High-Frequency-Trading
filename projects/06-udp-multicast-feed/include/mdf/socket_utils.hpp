#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace mdf {

class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) noexcept : fd_(fd) {}
  ~ScopedFd();

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  ScopedFd& operator=(ScopedFd&& other) noexcept;

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  void reset(int new_fd = -1) noexcept;
  int release() noexcept;

 private:
  int fd_ = -1;
};

bool set_socket_rcvbuf(int fd, int bytes);
bool set_socket_sndbuf(int fd, int bytes);
int get_socket_rcvbuf(int fd);
int get_socket_sndbuf(int fd);
bool set_nonblocking(int fd, bool non_blocking);
bool set_recv_timeout_ms(int fd, int timeout_ms);
bool set_send_timeout_ms(int fd, int timeout_ms);
bool disable_sigpipe(int fd);

// Linux-only SO_BUSY_POLL; returns false with explanation on macOS or unprivileged kernels.
bool set_busy_poll_us(int fd, int busy_poll_us, std::string* err_out = nullptr);

// Linux-only CPU affinity pinning; returns false on macOS.
bool pin_current_thread_to_core(int core_id, std::string* err_out = nullptr);

// Reads kernel UDP receive-buffer drop counter (Linux /proc/net/snmp Udp: RcvbufErrors).
// Returns std::nullopt on platforms where the counter is unavailable.
std::optional<uint64_t> read_kernel_udp_rcvbuf_errors();

// Reliable TCP stream helpers with poll-based timeout support.
bool send_all(int fd, std::span<const uint8_t> data, int timeout_ms = 5000);
bool recv_exact(int fd, uint8_t* dst, std::size_t len, int timeout_ms = 5000);

}  // namespace mdf
