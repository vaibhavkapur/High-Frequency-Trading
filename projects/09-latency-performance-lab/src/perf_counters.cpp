#include "lpl/perf_counters.hpp"

#include <unistd.h>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <cstring>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#endif

namespace lpl {

namespace {

#if defined(__linux__)
int open_hw_counter(uint64_t config, int group_fd) {
  struct perf_event_attr pe {};
  pe.type = PERF_TYPE_HARDWARE;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = config;
  pe.disabled = (group_fd == -1) ? 1 : 0;
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;
  return static_cast<int>(syscall(__NR_perf_event_open, &pe, 0, -1, group_fd, 0));
}

uint64_t read_fd_u64(int fd) {
  if (fd < 0) return 0;
  uint64_t val = 0;
  const ssize_t n = read(fd, &val, sizeof(val));
  return (n == static_cast<ssize_t>(sizeof(val))) ? val : 0;
}
#endif

}  // namespace

PerfCounterGroup::PerfCounterGroup() {
#if defined(__linux__)
  fd_cycles_ = open_hw_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
  if (fd_cycles_ >= 0) {
    fd_instructions_ = open_hw_counter(PERF_COUNT_HW_INSTRUCTIONS, fd_cycles_);
    fd_cache_refs_ = open_hw_counter(PERF_COUNT_HW_CACHE_REFERENCES, fd_cycles_);
    fd_cache_misses_ = open_hw_counter(PERF_COUNT_HW_CACHE_MISSES, fd_cycles_);
    fd_branches_ = open_hw_counter(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, fd_cycles_);
    fd_branch_misses_ = open_hw_counter(PERF_COUNT_HW_BRANCH_MISSES, fd_cycles_);
    backend_ = "linux_perf_event_open";
  } else {
    backend_ = "linux_perf_unavailable_check_paranoid";
  }
#elif defined(__APPLE__)
  rusage_info_v4 info {};
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0) {
    rusage_ok_ = true;
    backend_ = "macos_proc_pid_rusage";
  } else {
    rusage_ok_ = false;
    backend_ = "macos_rusage_unavailable";
  }
#else
  backend_ = "unavailable";
#endif
}

PerfCounterGroup::~PerfCounterGroup() {
#if defined(__linux__)
  const int fds[] = {fd_branch_misses_, fd_branches_, fd_cache_misses_,
                     fd_cache_refs_,    fd_instructions_, fd_cycles_};
  for (int fd : fds) {
    if (fd >= 0) close(fd);
  }
#endif
}

void PerfCounterGroup::start() {
#if defined(__linux__)
  if (fd_cycles_ >= 0) {
    ioctl(fd_cycles_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fd_cycles_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  }
#elif defined(__APPLE__)
  if (rusage_ok_) {
    rusage_info_v4 info {};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0) {
      start_cycles_ = info.ri_cycles;
      start_instructions_ = info.ri_instructions;
    }
  }
#endif
}

PerfSnapshot PerfCounterGroup::stop() {
  PerfSnapshot snap;
  snap.backend = backend_;

#if defined(__linux__)
  if (fd_cycles_ >= 0) {
    ioctl(fd_cycles_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    snap.cycles = read_fd_u64(fd_cycles_);
    snap.has_cycles = true;
    if (fd_instructions_ >= 0) {
      snap.instructions = read_fd_u64(fd_instructions_);
      snap.has_instructions = true;
    }
    if (fd_cache_refs_ >= 0 && fd_cache_misses_ >= 0) {
      snap.cache_references = read_fd_u64(fd_cache_refs_);
      snap.cache_misses = read_fd_u64(fd_cache_misses_);
      snap.has_cache_misses = true;
    }
    if (fd_branches_ >= 0 && fd_branch_misses_ >= 0) {
      snap.branches = read_fd_u64(fd_branches_);
      snap.branch_misses = read_fd_u64(fd_branch_misses_);
      snap.has_branch_misses = true;
    }
  }
#elif defined(__APPLE__)
  if (rusage_ok_) {
    rusage_info_v4 info {};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0) {
      if (info.ri_cycles >= start_cycles_ && info.ri_cycles > 0) {
        snap.cycles = info.ri_cycles - start_cycles_;
        snap.has_cycles = true;
      }
      if (info.ri_instructions >= start_instructions_ && info.ri_instructions > 0) {
        snap.instructions = info.ri_instructions - start_instructions_;
        snap.has_instructions = true;
      }
    }
  }
#endif

  return snap;
}

}  // namespace lpl
