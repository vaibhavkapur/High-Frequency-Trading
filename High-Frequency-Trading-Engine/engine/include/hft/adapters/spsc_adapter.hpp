#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "hft/contracts/v1/interfaces.hpp"
#include "lfm/affinity.hpp"
#include "lfm/spsc_queue.hpp"
#include "lfm/wait_policy.hpp"

namespace hft::adapters {

static_assert(std::is_nothrow_destructible_v<contracts::v1::Envelope>,
              "Envelope must be nothrow-destructible for lock-free SPSC queues");
static_assert(std::is_nothrow_move_assignable_v<contracts::v1::Envelope>,
              "Envelope must be nothrow-move-assignable for lock-free SPSC queues");

// Bounded wait-free SPSC transport wrapping Project 05's lfm::SpscQueue<Envelope>.
// Enforces explicit overflow policy:
//   - Backpressure: spins/yields up to max_backpressure_spins; never drops.
//   - Reject: fails immediately with ErrorCode::QueueFull.
//   - DropAndResync: ONLY allowed for MarketDataEvent payloads; marks resync_required_ and returns
//     ErrorCode::FeedGap. OrderIntent, ExecutionReport, and SystemEvent are NEVER silently dropped.
// Ownership contract: strictly ONE producer thread calls push(), ONE consumer thread calls try_pop().
class SpscTransportAdapter final : public contracts::v1::IEventTransport {
 public:
  SpscTransportAdapter(std::size_t capacity_power_of_two,
                       contracts::v1::QueueOverflowPolicy policy,
                       std::uint32_t max_backpressure_spins = 100'000);

  [[nodiscard]] contracts::v1::ErrorCode push(const contracts::v1::Envelope& env) noexcept override;
  [[nodiscard]] bool try_pop(contracts::v1::Envelope& out) noexcept override;

  [[nodiscard]] contracts::v1::QueueOverflowPolicy policy() const noexcept override {
    return policy_;
  }
  [[nodiscard]] contracts::v1::QueueStats stats() const noexcept override;

  [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }
  [[nodiscard]] std::size_t size_approx() const noexcept { return queue_.size_approx(); }
  [[nodiscard]] bool resync_required() const noexcept {
    return resync_required_.load(std::memory_order_acquire);
  }
  void clear_resync_required() noexcept {
    resync_required_.store(false, std::memory_order_release);
  }
  void request_stop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  static std::string affinity_capabilities();
  static bool hint_performance_core() noexcept;

 private:
  lfm::SpscQueue<contracts::v1::Envelope> queue_;
  contracts::v1::QueueOverflowPolicy policy_;
  std::uint32_t max_backpressure_spins_;
  std::atomic<bool> resync_required_{false};
  std::atomic<bool> stop_requested_{false};

  alignas(lfm::kCacheLine) std::atomic<std::uint64_t> pushed_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> dropped_for_resync_{0};
  std::atomic<std::uint64_t> backpressure_spins_{0};

  alignas(lfm::kCacheLine) std::atomic<std::uint64_t> popped_{0};
};

}  // namespace hft::adapters
