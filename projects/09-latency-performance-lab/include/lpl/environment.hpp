#pragma once

#include <cstdint>
#include <string>

namespace lpl {

struct EnvironmentInfo {
  std::string cpu_model = "unknown";
  std::string architecture = "unknown";
  int logical_cores = 0;
  int physical_cores = 0;
  int perf_cores = 0;
  int eff_cores = 0;
  uint64_t l1d_cache_bytes = 0;
  uint64_t l2_cache_bytes = 0;
  uint64_t l3_cache_bytes = 0;
  uint64_t cache_line_bytes = 64;
  std::string os_name = "unknown";
  std::string kernel_release = "unknown";
  std::string compiler_id = "unknown";
  std::string compiler_version = "unknown";
  std::string build_type = "unknown";
  std::string cxx_flags = "";
  bool march_native = false;
  std::string cpu_governor = "unknown";
  std::string turbo_status = "unknown";
  double clock_overhead_ns = 0.0;
  std::string timestamp_utc = "";
};

EnvironmentInfo collect_environment_info();
std::string environment_to_json(const EnvironmentInfo& env);
std::string environment_to_markdown(const EnvironmentInfo& env);

}  // namespace lpl
