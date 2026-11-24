// Minimal non-blocking TCP helpers over POSIX sockets (Linux and macOS).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ouch::net {

// Listens on 127.0.0.1:`port` (0 = ephemeral). Returns the fd or -1; the bound
// port is written to `bound_port`.
int tcp_listen(std::uint16_t port, std::uint16_t& bound_port);

// Blocking connect to an IPv4 address, then switches the socket to
// non-blocking. Returns the fd or -1.
int tcp_connect(const std::string& host, std::uint16_t port, bool nodelay);

// Accepts one pending connection as a non-blocking socket, or -1.
int tcp_accept(int listen_fd, bool nodelay);

void close_fd(int& fd);

// A connected socket with a user-space send buffer.
class Conn {
 public:
  Conn() = default;
  explicit Conn(int fd) : fd_(fd) {}
  ~Conn() { close(); }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  Conn(Conn&& o) noexcept;
  Conn& operator=(Conn&& o) noexcept;

  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }
  void close();

  // Bytes are appended here and pushed by flush().
  std::vector<std::uint8_t>& out() { return out_; }
  [[nodiscard]] std::size_t pending() const { return out_.size() - out_off_; }

  // Writes as much buffered data as the socket accepts. False on a hard error.
  bool flush();

  // Reads available bytes into `buf`. Returns bytes read (>0), 0 if nothing is
  // available right now, or -1 on EOF / error.
  long read_some(std::span<std::uint8_t> buf);

 private:
  int fd_ = -1;
  std::vector<std::uint8_t> out_;
  std::size_t out_off_ = 0;
};

}  // namespace ouch::net
