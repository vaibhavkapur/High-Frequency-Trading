#pragma once

#include <cstdint>
#include <vector>

#include "hft/contracts/v1/events.hpp"

namespace hft::runtime {

// Deterministic execution-path fault injector for Phase 3 correctness & recovery verification.
// Injects repeatable duplicate execution reports, delayed acks, dropped responses (stored for
// reconnect replay), and session disconnects.
struct ExecFaultConfig {
  std::uint64_t seed = 42;
  bool duplicate_every_fill = false;
  std::uint32_t duplicate_report_every_n = 0;  // 0 = disabled
  std::int64_t extra_ack_delay_ns = 0;
  std::uint32_t drop_response_at_index = 0;    // 1-based report index to drop until reconnect
  std::uint32_t disconnect_after_reports = 0;  // trigger disconnect after N reports
};

struct FaultInjectionResult {
  bool drop_report = false;
  bool emit_duplicate = false;
  bool trigger_disconnect = false;
  std::int64_t extra_delay_ns = 0;
};

struct ExecFaultStats {
  std::uint64_t reports_seen = 0;
  std::uint64_t duplicates_injected = 0;
  std::uint64_t responses_dropped = 0;
  std::uint64_t acks_delayed = 0;
  std::uint64_t disconnects_injected = 0;
};

class ExecutionFaultInjector {
 public:
  explicit ExecutionFaultInjector(ExecFaultConfig cfg = {});

  FaultInjectionResult evaluate(contracts::v1::InstrumentId instrument,
                                const contracts::v1::ExecutionReport& report);

  [[nodiscard]] const std::vector<
      std::pair<contracts::v1::InstrumentId, contracts::v1::ExecutionReport>>&
  buffered_for_reconnect() const noexcept {
    return dropped_for_reconnect_;
  }
  void clear_reconnect_buffer() noexcept { dropped_for_reconnect_.clear(); }
  [[nodiscard]] const ExecFaultStats& stats() const noexcept { return stats_; }

 private:
  ExecFaultConfig cfg_;
  ExecFaultStats stats_{};
  std::vector<std::pair<contracts::v1::InstrumentId, contracts::v1::ExecutionReport>>
      dropped_for_reconnect_;
};

}  // namespace hft::runtime
