#pragma once

#include <string>

namespace lfm {

// Hard-pins the calling thread to one logical CPU.
// Linux: pthread_setaffinity_np. Returns false on failure.
// macOS: always returns false - Apple Silicon exposes no hard affinity API.
bool pin_current_thread(int cpu) noexcept;

enum class CoreClass { kPerformance, kEfficiency };

// Scheduler *hint* for asymmetric CPUs.
// macOS: sets the thread QoS class (user-interactive prefers P-cores,
// background is restricted to E-cores). This is a preference, not a pin.
// Linux: no-op, returns false (use pin_current_thread instead).
bool hint_core_class(CoreClass cls) noexcept;

// Logical CPU the caller is running on right now, or -1 if unknown (macOS).
int current_cpu() noexcept;

// One-line description of what this platform supports, for benchmark logs.
std::string affinity_support();

}  // namespace lfm
