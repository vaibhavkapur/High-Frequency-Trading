#pragma once

#include <cstdint>
#include <string>

namespace lpl {

struct PerfSnapshot {
  std::string backend = "unavailable";
  bool has_cycles = false;
  bool has_instructions = false;
  bool has_cache_misses = false;
  bool has_branch_misses = false;

  uint64_t cycles = 0;
  uint64_t instructions = 0;
  uint64_t cache_references = 0;
  uint64_t cache_misses = 0;
  uint64_t branches = 0;
  uint64_t branch_misses = 0;

  double ipc() const noexcept {
    return (has_cycles && has_instructions && cycles > 0)
               ? static_cast<double>(instructions) / static_cast<double>(cycles)
               : -1.0;
  }

  double cache_miss_rate() const noexcept {
    return (has_cache_misses && cache_references > 0)
               ? static_cast<double>(cache_misses) / static_cast<double>(cache_references)
               : -1.0;
  }

  double branch_miss_rate() const noexcept {
    return (has_branch_misses && branches > 0)
               ? static_cast<double>(branch_misses) / static_cast<double>(branches)
               : -1.0;
  }
};

// Cross-platform hardware performance counter reader.
// - Linux: uses perf_event_open(2) for cycles, instructions, LLC misses, branch misses.
// - macOS: uses proc_pid_rusage(RUSAGE_INFO_V4) for hardware cycles and instructions.
//   (LLC and branch miss counters require Linux perf_event_open or privileged macOS kpc.)
class PerfCounterGroup {
 public:
  PerfCounterGroup();
  ~PerfCounterGroup();

  PerfCounterGroup(const PerfCounterGroup&) = delete;
  PerfCounterGroup& operator=(const PerfCounterGroup&) = delete;

  void start();
  PerfSnapshot stop();

  const std::string& backend_name() const noexcept { return backend_; }

 private:
  std::string backend_ = "unavailable";

#if defined(__linux__)
  int fd_cycles_ = -1;
  int fd_instructions_ = -1;
  int fd_cache_refs_ = -1;
  int fd_cache_misses_ = -1;
  int fd_branches_ = -1;
  int fd_branch_misses_ = -1;
#elif defined(__APPLE__)
  uint64_t start_cycles_ = 0;
  uint64_t start_instructions_ = 0;
  bool rusage_ok_ = false;
#endif
};

}  // namespace lpl
