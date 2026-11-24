#include "ouch/event_log.hpp"

#include <cstring>
#include <variant>

namespace ouch {
namespace {

inline constexpr std::size_t kHeaderSize = 4 + 8 + 1 + 8;
inline constexpr std::uint32_t kMaxPayload = 1 << 16;

void put_le(std::uint8_t* p, std::uint64_t v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint64_t get_le(const std::uint8_t* p, std::size_t n) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace

EventLog::~EventLog() { close(); }

bool EventLog::open(const std::string& path, bool flush_each) {
  close();
  file_ = std::fopen(path.c_str(), "ab");
  if (file_ == nullptr) return false;
  flush_each_ = flush_each;
  std::uint8_t wall[8];
  put_le(wall, wall_ns(), 8);
  append(RecordKind::Start, now_ns(), 0, wall);
  return true;
}

void EventLog::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

void EventLog::append(RecordKind kind, std::uint64_t ts_ns, std::uint64_t seq,
                      std::span<const std::uint8_t> payload) {
  if (file_ == nullptr) return;
  std::uint8_t hdr[kHeaderSize];
  put_le(hdr, payload.size(), 4);
  put_le(hdr + 4, ts_ns, 8);
  hdr[12] = static_cast<std::uint8_t>(kind);
  put_le(hdr + 13, seq, 8);
  std::fwrite(hdr, 1, kHeaderSize, file_);
  if (!payload.empty()) std::fwrite(payload.data(), 1, payload.size(), file_);
  if (flush_each_) std::fflush(file_);
  ++written_;
}

void EventLog::flush() {
  if (file_ != nullptr) std::fflush(file_);
}

bool EventLog::read_all(const std::string& path, std::vector<Record>& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::uint8_t hdr[kHeaderSize];
  while (std::fread(hdr, 1, kHeaderSize, f) == kHeaderSize) {
    const auto len = static_cast<std::uint32_t>(get_le(hdr, 4));
    if (len > kMaxPayload) break;  // corrupt length: stop at the last good record
    Record r;
    r.ts_ns = get_le(hdr + 4, 8);
    r.kind = static_cast<RecordKind>(hdr[12]);
    r.seq = get_le(hdr + 13, 8);
    r.payload.resize(len);
    if (len > 0 && std::fread(r.payload.data(), 1, len, f) != len) break;  // torn tail
    out.push_back(std::move(r));
  }
  std::fclose(f);
  return true;
}

ReplayResult replay(const std::vector<Record>& records, OrderManager& oms) {
  ReplayResult res;
  for (const Record& r : records) {
    switch (r.kind) {
      case RecordKind::Start:
      case RecordKind::RiskReject:
        break;
      case RecordKind::RequestSent: {
        wire::InboundMessage m;
        if (wire::decode_inbound(r.payload, m) == wire::DecodeError::None &&
            oms.on_request_sent(m, r.ts_ns)) {
          ++res.applied;
        } else {
          ++res.skipped;
        }
        break;
      }
      case RecordKind::RequestResent: {
        wire::InboundMessage m;
        if (wire::decode_inbound(r.payload, m) != wire::DecodeError::None) {
          ++res.skipped;
          break;
        }
        if (const auto* e = std::get_if<wire::EnterOrder>(&m)) {
          oms.on_resent(e->token);
        } else if (const auto* u = std::get_if<wire::ReplaceOrder>(&m)) {
          oms.on_resent(u->replacement_token);
        } else if (const auto* x = std::get_if<wire::CancelOrder>(&m)) {
          oms.on_cancel_resent(x->token);
        }
        ++res.applied;
        break;
      }
      case RecordKind::ExchangeMsg: {
        res.next_seq = r.seq + 1;
        wire::OutboundMessage m;
        if (wire::decode_outbound(r.payload, m) == wire::DecodeError::None) {
          oms.on_exchange(m, r.ts_ns);
          ++res.applied;
        } else {
          ++res.skipped;
        }
        break;
      }
      case RecordKind::SessionUp:
        // A login implies any earlier session ended, even if the process died
        // before it could journal a SessionDown.
        oms.on_disconnect();
        res.next_seq = r.seq;
        res.session.assign(r.payload.begin(), r.payload.end());
        ++res.applied;
        break;
      case RecordKind::SessionDown:
        oms.on_disconnect();
        ++res.applied;
        break;
      default:
        ++res.skipped;
        break;
    }
  }
  return res;
}

}  // namespace ouch
