#include "hft/adapters/spsc_adapter.hpp"

namespace hft::adapters {

using namespace contracts::v1;

SpscTransportAdapter::SpscTransportAdapter(std::size_t capacity_power_of_two,
                                           QueueOverflowPolicy policy,
                                           std::uint32_t max_backpressure_spins)
    : queue_(capacity_power_of_two),
      policy_(policy),
      max_backpressure_spins_(max_backpressure_spins) {}

ErrorCode SpscTransportAdapter::push(const Envelope& env) noexcept {
  if (queue_.try_push(env)) {
    pushed_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::Ok;
  }

  if (policy_ == QueueOverflowPolicy::Reject) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::QueueFull;
  }

  if (policy_ == QueueOverflowPolicy::DropAndResync) {
    // Safety invariant: NEVER silently drop OrderIntent, ExecutionReport, or SystemEvent.
    if (std::holds_alternative<MarketDataEvent>(env.payload)) {
      dropped_for_resync_.fetch_add(1, std::memory_order_relaxed);
      resync_required_.store(true, std::memory_order_release);
      return ErrorCode::FeedGap;
    }
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::QueueFull;
  }

  // QueueOverflowPolicy::Backpressure
  lfm::SpinThenYield wait{1024};
  wait.reset();
  for (std::uint32_t i = 0; i < max_backpressure_spins_; ++i) {
    if (stop_requested_.load(std::memory_order_acquire)) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::QueueFull;
    }
    backpressure_spins_.fetch_add(1, std::memory_order_relaxed);
    wait.wait();
    if (queue_.try_push(env)) {
      pushed_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::Ok;
    }
  }
  rejected_.fetch_add(1, std::memory_order_relaxed);
  return ErrorCode::QueueFull;
}

bool SpscTransportAdapter::try_pop(Envelope& out) noexcept {
  if (queue_.try_pop(out)) {
    popped_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

QueueStats SpscTransportAdapter::stats() const noexcept {
  QueueStats s{};
  s.pushed = pushed_.load(std::memory_order_relaxed);
  s.popped = popped_.load(std::memory_order_relaxed);
  s.rejected = rejected_.load(std::memory_order_relaxed);
  s.dropped_for_resync = dropped_for_resync_.load(std::memory_order_relaxed);
  s.backpressure_spins = backpressure_spins_.load(std::memory_order_relaxed);
  return s;
}

std::string SpscTransportAdapter::affinity_capabilities() { return lfm::affinity_support(); }

bool SpscTransportAdapter::hint_performance_core() noexcept {
  return lfm::hint_core_class(lfm::CoreClass::kPerformance);
}

}  // namespace hft::adapters
