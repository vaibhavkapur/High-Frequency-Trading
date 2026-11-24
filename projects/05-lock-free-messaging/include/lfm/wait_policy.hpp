#pragma once

#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace lfm {

// Hint to the CPU that we are in a spin-wait loop. This never yields to the
// OS scheduler.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  // `isb` gives a short, predictable delay; `yield` is a no-op on most cores.
  __asm__ __volatile__("isb");
#endif
}

// Wait policies decide what a thread does between failed attempts. They are
// only used by the blocking helpers in endpoints.hpp; try_push/try_pop never
// wait.
//
//   reset() - called once before a wait loop begins
//   wait()  - called after each failed attempt

// Burn the core. Lowest wake-up latency, 100% CPU, and starves the peer if
// both threads share one hardware thread.
struct BusySpin {
  void reset() noexcept {}
  void wait() noexcept { cpu_relax(); }
};

// Give the scheduler a chance on every failed attempt.
struct YieldWait {
  void reset() noexcept {}
  void wait() noexcept { std::this_thread::yield(); }
};

// Spin for a bounded number of attempts, then fall back to yielding.
struct SpinThenYield {
  std::uint32_t spin_limit = 1024;
  std::uint32_t spins = 0;

  void reset() noexcept { spins = 0; }
  void wait() noexcept {
    if (spins < spin_limit) {
      ++spins;
      cpu_relax();
    } else {
      std::this_thread::yield();
    }
  }
};

}  // namespace lfm
