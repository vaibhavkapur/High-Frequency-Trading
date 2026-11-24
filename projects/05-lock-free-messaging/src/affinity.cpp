#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "lfm/affinity.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace lfm {

#if defined(__linux__)

bool pin_current_thread(int cpu) noexcept {
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

bool hint_core_class(CoreClass) noexcept { return false; }

int current_cpu() noexcept { return sched_getcpu(); }

std::string affinity_support() { return "linux: hard affinity via pthread_setaffinity_np"; }

#elif defined(__APPLE__)

bool pin_current_thread(int) noexcept { return false; }

bool hint_core_class(CoreClass cls) noexcept {
  const qos_class_t qos =
      cls == CoreClass::kPerformance ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND;
  return pthread_set_qos_class_self_np(qos, 0) == 0;
}

int current_cpu() noexcept { return -1; }

std::string affinity_support() {
  return "macos: no hard affinity; QoS class hint only (P-core vs E-core preference)";
}

#else

bool pin_current_thread(int) noexcept { return false; }
bool hint_core_class(CoreClass) noexcept { return false; }
int current_cpu() noexcept { return -1; }
std::string affinity_support() { return "unsupported platform: no affinity control"; }

#endif

}  // namespace lfm
