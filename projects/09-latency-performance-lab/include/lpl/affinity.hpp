#pragma once

#include <cstdint>
#include <string>

namespace lpl {

struct AffinityResult {
  bool requested = false;
  bool applied = false;
  bool strict_core_pin = false;
  int core_id = -1;
  std::string note;
};

// Pin the calling thread to `core_id` (>= 0).
// On Linux, applies hard CPU affinity via pthread_setaffinity_np.
// On macOS, applies QoS + THREAD_AFFINITY_POLICY hint (XNU does not expose hard core pinning).
AffinityResult pin_current_thread(int core_id);

// Low-power spin-loop hint (ARM64 yield / x86 pause).
inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
  asm volatile("pause" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

// Monotonic per-thread CPU time in nanoseconds.
uint64_t current_thread_cpu_ns() noexcept;

// Process-wide user + system CPU time in nanoseconds.
uint64_t process_cpu_ns() noexcept;

}  // namespace lpl
