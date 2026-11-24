#include "lpl/affinity.hpp"

#include <sys/resource.h>
#include <time.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstring>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace lpl {

AffinityResult pin_current_thread(int core_id) {
  AffinityResult res;
  if (core_id < 0) {
    res.requested = false;
    res.note = "none (OS scheduler default)";
    return res;
  }

  res.requested = true;
  res.core_id = core_id;

#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<size_t>(core_id), &cpuset);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc == 0) {
    res.applied = true;
    res.strict_core_pin = true;
    res.note = "pinned to Linux CPU " + std::to_string(core_id);
  } else {
    res.applied = false;
    res.note = std::string("pthread_setaffinity_np failed: ") + std::strerror(rc);
  }
#elif defined(__APPLE__)
  // macOS XNU does not allow hard core pinning from userspace; request interactive QoS
  // (prefers P-cores on Apple Silicon) plus an affinity tag for cache locality.
  (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  thread_affinity_policy_data_t policy = {core_id + 1};
  kern_return_t kr = thread_policy_set(
      mach_thread_self(), THREAD_AFFINITY_POLICY,
      reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
  res.applied = (kr == KERN_SUCCESS);
  res.strict_core_pin = false;
  res.note = "macOS QoS=USER_INTERACTIVE + affinity_tag=" + std::to_string(core_id + 1) +
             " (hard core pinning unsupported on XNU)";
#else
  res.applied = false;
  res.note = "thread pinning unsupported on this OS";
#endif

  return res;
}

uint64_t current_thread_cpu_ns() noexcept {
  struct timespec ts {};
#if defined(CLOCK_THREAD_CPUTIME_ID)
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }
#endif
  return process_cpu_ns();
}

uint64_t process_cpu_ns() noexcept {
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
  const uint64_t user_ns = static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1'000'000'000ULL +
                           static_cast<uint64_t>(ru.ru_utime.tv_usec) * 1'000ULL;
  const uint64_t sys_ns = static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ru.ru_stime.tv_usec) * 1'000ULL;
  return user_ns + sys_ns;
}

}  // namespace lpl
