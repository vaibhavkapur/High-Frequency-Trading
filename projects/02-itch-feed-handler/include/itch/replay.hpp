// Replay pacing ("basic throughput control").
//
// A Pacer decides when message k may be released. Event *order* is never
// affected - messages are always delivered in file order - only timing.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#include "itch/messages.hpp"

namespace itch {

class Pacer {
 public:
  enum class Mode : std::uint8_t {
    Unlimited,   // as fast as possible
    FixedRate,   // N messages per wall-clock second
    TimeScaled,  // follow ITCH timestamps, sped up by a factor
  };

  static Pacer unlimited() noexcept { return Pacer{Mode::Unlimited, 0.0}; }
  static Pacer fixed_rate(double messages_per_second) noexcept {
    return Pacer{Mode::FixedRate, messages_per_second};
  }
  static Pacer time_scaled(double speed) noexcept { return Pacer{Mode::TimeScaled, speed}; }

  // Nanoseconds after replay start at which the message with 0-based index
  // `index` and ITCH timestamp `ts` is due. Pure, so it can be unit tested
  // without sleeping.
  [[nodiscard]] std::uint64_t due_ns(std::uint64_t index, Timestamp ts) noexcept {
    switch (mode_) {
      case Mode::Unlimited:
        return 0;
      case Mode::FixedRate:
        return static_cast<std::uint64_t>(static_cast<double>(index) * 1e9 / param_);
      case Mode::TimeScaled:
        if (!have_first_) {
          have_first_ = true;
          first_ts_ = ts;
        }
        // Timestamps that step backwards (or before the first one) are due now.
        if (ts <= first_ts_) return 0;
        return static_cast<std::uint64_t>(static_cast<double>(ts - first_ts_) / param_);
    }
    return 0;
  }

  // Blocks until the message is due.
  void wait(std::uint64_t index, Timestamp ts) {
    if (mode_ == Mode::Unlimited) return;
    using clock = std::chrono::steady_clock;
    if (!started_) {
      started_ = true;
      start_ = clock::now();
    }
    const auto due = start_ + std::chrono::nanoseconds(due_ns(index, ts));
    if (due > clock::now()) std::this_thread::sleep_until(due);
  }

  [[nodiscard]] Mode mode() const noexcept { return mode_; }

 private:
  Pacer(Mode mode, double param) noexcept : mode_(mode), param_(param) {}

  Mode mode_;
  double param_;
  bool have_first_ = false;
  bool started_ = false;
  Timestamp first_ts_ = 0;
  std::chrono::steady_clock::time_point start_{};
};

}  // namespace itch
