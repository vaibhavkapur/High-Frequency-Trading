#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

#include "hft/contracts/v1/events.hpp"

namespace hft::runtime {

// Authoritative deterministic discrete-event scheduler (ADR-002).
// Orders all events strictly by EventOrderKey:
//   (event_time_ns, priority_class, source_sequence, insertion_sequence).
// Guarantees monotonic simulated time (never steps backward).
class DeterministicScheduler {
 public:
  struct ScheduledItem {
    contracts::v1::EventOrderKey key{};
    contracts::v1::Envelope envelope{};
    std::function<void(const contracts::v1::Envelope&)> callback;

    bool operator>(const ScheduledItem& other) const noexcept { return key > other.key; }
  };

  void schedule(contracts::v1::SimTimeNs event_time_ns, contracts::v1::PriorityClass priority,
                contracts::v1::EventSeq source_sequence, contracts::v1::Envelope envelope,
                std::function<void(const contracts::v1::Envelope&)> callback);

  [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
  [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
  [[nodiscard]] contracts::v1::SimTimeNs now() const noexcept { return current_time_ns_; }
  [[nodiscard]] std::uint64_t processed_count() const noexcept { return processed_count_; }
  [[nodiscard]] std::uint64_t causality_violations() const noexcept {
    return causality_violations_;
  }

  bool step();
  void run_all();

 private:
  std::priority_queue<ScheduledItem, std::vector<ScheduledItem>, std::greater<ScheduledItem>>
      queue_;
  contracts::v1::EventSeq next_insertion_seq_ = 1;
  contracts::v1::SimTimeNs current_time_ns_{0};
  std::uint64_t processed_count_ = 0;
  std::uint64_t causality_violations_ = 0;
};

}  // namespace hft::runtime
