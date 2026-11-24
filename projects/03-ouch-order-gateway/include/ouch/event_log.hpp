// Append-only journal of everything that can change order state.
//
// Records are written *before* the corresponding bytes go on the wire
// (write-ahead), so after a crash the journal is a superset of what the
// exchange may have seen. Anything journaled but unanswered is treated as
// uncertain and reconciled on the next login.
//
// On-disk record (integers little-endian, the file is local to one host):
//   u32 payload_len | u64 ts_ns (monotonic) | u8 kind | u64 seq | payload
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "ouch/order_manager.hpp"

namespace ouch {

enum class RecordKind : std::uint8_t {
  Start = 'S',          // journal opened; payload = u64 wall-clock ns (anchors ts_ns)
  RequestSent = 'O',    // payload = encoded OUCH inbound message
  RequestResent = 'R',  // payload = encoded OUCH inbound message (same token as before)
  ExchangeMsg = 'I',    // seq = SoupBinTCP sequence number; payload = OUCH outbound message
  SessionUp = 'U',      // seq = next expected sequence number; payload = 10-char session id
  SessionDown = 'D',
  RiskReject = 'X',  // payload = 1 reason byte + the encoded request that was refused
};

struct Record {
  std::uint64_t ts_ns = 0;
  RecordKind kind = RecordKind::Start;
  std::uint64_t seq = 0;
  std::vector<std::uint8_t> payload;
};

class EventLog {
 public:
  EventLog() = default;
  ~EventLog();
  EventLog(const EventLog&) = delete;
  EventLog& operator=(const EventLog&) = delete;

  // Opens for append (creating the file if needed). With flush_each, every
  // record is handed to the OS before append() returns; otherwise records sit
  // in a user-space buffer until flush() or close().
  bool open(const std::string& path, bool flush_each);
  void close();
  [[nodiscard]] bool is_open() const { return file_ != nullptr; }

  void append(RecordKind kind, std::uint64_t ts_ns, std::uint64_t seq,
              std::span<const std::uint8_t> payload = {});
  void flush();
  [[nodiscard]] std::uint64_t records_written() const { return written_; }

  // Reads every complete record. A truncated final record (torn write) is
  // ignored. Returns false only if the file cannot be opened.
  static bool read_all(const std::string& path, std::vector<Record>& out);

 private:
  std::FILE* file_ = nullptr;
  bool flush_each_ = false;
  std::uint64_t written_ = 0;
};

// What replaying a journal tells a restarting gateway.
struct ReplayResult {
  std::uint64_t next_seq = 1;  // next SoupBinTCP sequence number to request
  std::string session;         // last session id logged in to (10 chars, or empty)
  std::uint64_t applied = 0;   // records applied to the OrderManager
  std::uint64_t skipped = 0;   // records whose payload could not be decoded or applied
};

// Applies journal records to `oms` in order. The caller decides what the end
// of the journal means (a restarting gateway treats it as a disconnect).
ReplayResult replay(const std::vector<Record>& records, OrderManager& oms);

}  // namespace ouch
