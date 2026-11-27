#include "hft/runtime/scheduler.hpp"

namespace hft::runtime {

using namespace contracts::v1;

void DeterministicScheduler::schedule(SimTimeNs event_time_ns, PriorityClass priority,
                                      EventSeq source_sequence, Envelope envelope,
                                      std::function<void(const Envelope&)> callback) {
  if (event_time_ns < current_time_ns_) {
    ++causality_violations_;
    event_time_ns = current_time_ns_;
  }
  ScheduledItem item{};
  item.key = EventOrderKey{event_time_ns.value, priority, source_sequence, next_insertion_seq_++};
  item.envelope = std::move(envelope);
  item.envelope.event_time_ns = event_time_ns;
  item.envelope.priority = priority;
  item.callback = std::move(callback);
  queue_.push(std::move(item));
}

bool DeterministicScheduler::step() {
  if (queue_.empty()) return false;
  ScheduledItem item = std::move(const_cast<ScheduledItem&>(queue_.top()));
  queue_.pop();
  if (item.key.event_time_ns < current_time_ns_.value) {
    ++causality_violations_;
  } else {
    current_time_ns_ = SimTimeNs{item.key.event_time_ns};
  }
  ++processed_count_;
  if (item.callback) {
    item.callback(item.envelope);
  }
  return true;
}

void DeterministicScheduler::run_all() {
  while (step()) {
  }
}

}  // namespace hft::runtime
