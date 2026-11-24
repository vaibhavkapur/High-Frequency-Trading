#include "ouch/soupbin.hpp"

#include <cstring>

namespace ouch::soup {
namespace {

inline constexpr std::size_t kSeqWidth = 20;

// Numeric fields are ASCII digits, right-justified and space-padded on the left.
void put_numeric(std::uint8_t* p, std::uint64_t v) {
  std::memset(p, ' ', kSeqWidth);
  std::size_t i = kSeqWidth;
  do {
    p[--i] = static_cast<std::uint8_t>('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
}

bool get_numeric(const std::uint8_t* p, std::uint64_t& out) {
  std::size_t i = 0;
  while (i < kSeqWidth && p[i] == ' ') ++i;
  if (i == kSeqWidth) return false;
  std::uint64_t v = 0;
  for (; i < kSeqWidth; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
    const std::uint64_t d = static_cast<std::uint64_t>(p[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;  // overflow
    v = v * 10 + d;
  }
  out = v;
  return true;
}

}  // namespace

bool append_packet(std::vector<std::uint8_t>& out, char type,
                   std::span<const std::uint8_t> payload) {
  if (payload.size() > 0xFFFE) return false;
  const std::size_t len = payload.size() + 1;
  const std::size_t at = out.size();
  out.resize(at + 2 + len);
  out[at] = static_cast<std::uint8_t>(len >> 8);
  out[at + 1] = static_cast<std::uint8_t>(len);
  out[at + 2] = static_cast<std::uint8_t>(type);
  if (!payload.empty()) std::memcpy(out.data() + at + 3, payload.data(), payload.size());
  return true;
}

void append_login_request(std::vector<std::uint8_t>& out, const LoginRequest& m) {
  std::uint8_t p[LoginRequest::kSize];
  std::memcpy(p, m.username.c.data(), 6);
  std::memcpy(p + 6, m.password.c.data(), 10);
  std::memcpy(p + 16, m.session.c.data(), 10);
  put_numeric(p + 26, m.seq);
  append_packet(out, kLoginRequest, p);
}

void append_login_accepted(std::vector<std::uint8_t>& out, const LoginAccepted& m) {
  std::uint8_t p[LoginAccepted::kSize];
  std::memcpy(p, m.session.c.data(), 10);
  put_numeric(p + 10, m.seq);
  append_packet(out, kLoginAccepted, p);
}

void append_login_rejected(std::vector<std::uint8_t>& out, char reason) {
  const std::uint8_t p[1] = {static_cast<std::uint8_t>(reason)};
  append_packet(out, kLoginRejected, p);
}

bool parse_login_request(std::span<const std::uint8_t> payload, LoginRequest& out) {
  if (payload.size() != LoginRequest::kSize) return false;
  std::memcpy(out.username.c.data(), payload.data(), 6);
  std::memcpy(out.password.c.data(), payload.data() + 6, 10);
  std::memcpy(out.session.c.data(), payload.data() + 16, 10);
  return get_numeric(payload.data() + 26, out.seq);
}

bool parse_login_accepted(std::span<const std::uint8_t> payload, LoginAccepted& out) {
  if (payload.size() != LoginAccepted::kSize) return false;
  std::memcpy(out.session.c.data(), payload.data(), 10);
  return get_numeric(payload.data() + 10, out.seq);
}

void FrameParser::feed(std::span<const std::uint8_t> bytes) {
  if (error_ || bytes.empty()) return;
  // Compact consumed bytes before growing.
  if (off_ > 0) {
    if (off_ == buf_.size()) {
      buf_.clear();
    } else if (off_ >= 4096) {
      buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off_));
    } else {
      buf_.insert(buf_.end(), bytes.begin(), bytes.end());
      return;
    }
    off_ = 0;
  }
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

FrameParser::Status FrameParser::next(char& type, std::span<const std::uint8_t>& payload) {
  if (error_) return Status::Error;
  const std::size_t avail = buf_.size() - off_;
  if (avail < 2) return Status::NeedMore;
  const std::size_t len = (static_cast<std::size_t>(buf_[off_]) << 8) | buf_[off_ + 1];
  if (len == 0) {
    error_ = true;
    return Status::Error;
  }
  if (avail < 2 + len) return Status::NeedMore;
  type = static_cast<char>(buf_[off_ + 2]);
  payload = std::span<const std::uint8_t>(buf_.data() + off_ + 3, len - 1);
  off_ += 2 + len;
  return Status::Packet;
}

void FrameParser::reset() {
  buf_.clear();
  off_ = 0;
  error_ = false;
}

}  // namespace ouch::soup
