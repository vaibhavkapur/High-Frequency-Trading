#include "lpl/environment.hpp"

#include <sys/utsname.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "lpl/bench_harness.hpp"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace lpl {

namespace {

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

#if defined(__linux__)
std::string read_first_line(const char* path) {
  std::ifstream in(path);
  if (!in.is_open()) return "";
  std::string line;
  std::getline(in, line);
  return trim(line);
}
#endif

#if defined(__APPLE__)
std::string sysctl_string(const char* name) {
  size_t len = 0;
  if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) {
    return "";
  }
  std::string buf(len, '\0');
  if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) {
    return "";
  }
  while (!buf.empty() && buf.back() == '\0') {
    buf.pop_back();
  }
  return buf;
}

uint64_t sysctl_u64(const char* name) {
  uint64_t val = 0;
  size_t len = sizeof(val);
  if (sysctlbyname(name, &val, &len, nullptr, 0) == 0) {
    return val;
  }
  uint32_t val32 = 0;
  len = sizeof(val32);
  if (sysctlbyname(name, &val32, &len, nullptr, 0) == 0) {
    return val32;
  }
  return 0;
}
#endif

std::string current_utc_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm utc {};
#if defined(_WIN32)
  gmtime_s(&utc, &tt);
#else
  gmtime_r(&tt, &utc);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

}  // namespace

EnvironmentInfo collect_environment_info() {
  EnvironmentInfo env;
  env.timestamp_utc = current_utc_iso8601();
  env.logical_cores = static_cast<int>(std::thread::hardware_concurrency());
  env.physical_cores = env.logical_cores;

  struct utsname uts {};
  if (uname(&uts) == 0) {
    env.os_name = uts.sysname;
    env.kernel_release = uts.release;
    env.architecture = uts.machine;
  }

#ifdef LPL_COMPILER_ID_OVERRIDE
  env.compiler_id = LPL_COMPILER_ID_OVERRIDE;
#elif defined(LPL_CXX_COMPILER_ID_STR)
  env.compiler_id = LPL_CXX_COMPILER_ID_STR;
#endif

#ifdef LPL_CXX_COMPILER_VER_STR
  env.compiler_version = LPL_CXX_COMPILER_VER_STR;
#endif

#ifdef LPL_BUILD_TYPE_STR
  env.build_type = LPL_BUILD_TYPE_STR;
#endif

#ifdef LPL_CXX_FLAGS_STR
  env.cxx_flags = trim(LPL_CXX_FLAGS_STR);
#endif

#ifdef LPL_MARCH_NATIVE_ENABLED
  env.march_native = (LPL_MARCH_NATIVE_ENABLED != 0);
#endif

#if defined(__APPLE__)
  env.cpu_model = sysctl_string("machdep.cpu.brand_string");
  if (env.cpu_model.empty()) {
    env.cpu_model = "Apple Silicon";
  }
  env.physical_cores = static_cast<int>(sysctl_u64("hw.physicalcpu"));
  env.logical_cores = static_cast<int>(sysctl_u64("hw.logicalcpu"));
  env.perf_cores = static_cast<int>(sysctl_u64("hw.perflevel0.physicalcpu"));
  env.eff_cores = static_cast<int>(sysctl_u64("hw.perflevel1.physicalcpu"));
  env.l1d_cache_bytes = sysctl_u64("hw.l1dcachesize");
  env.l2_cache_bytes = sysctl_u64("hw.l2cachesize");
  env.l3_cache_bytes = sysctl_u64("hw.l3cachesize");
  const uint64_t cls = sysctl_u64("hw.cachelinesize");
  if (cls > 0) env.cache_line_bytes = cls;
  const std::string prod_ver = sysctl_string("kern.osproductversion");
  if (!prod_ver.empty()) {
    env.os_name = "macOS " + prod_ver;
  }
  env.cpu_governor = "macOS DVFS (OS-managed)";
  env.turbo_status = "OS-managed (DVFS)";
#elif defined(__linux__)
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.rfind("model name", 0) == 0) {
        const auto pos = line.find(':');
        if (pos != std::string::npos) {
          env.cpu_model = trim(line.substr(pos + 1));
          break;
        }
      }
    }
  }
  {
    const std::string gov =
        read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    env.cpu_governor = gov.empty() ? "unavailable" : gov;
  }
  {
    const std::string intel_no_turbo =
        read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
    const std::string cpufreq_boost = read_first_line("/sys/devices/system/cpu/cpufreq/boost");
    if (!intel_no_turbo.empty()) {
      env.turbo_status = (intel_no_turbo == "1") ? "disabled (no_turbo=1)" : "enabled (no_turbo=0)";
    } else if (!cpufreq_boost.empty()) {
      env.turbo_status = (cpufreq_boost == "1") ? "enabled (boost=1)" : "disabled (boost=0)";
    } else {
      env.turbo_status = "unavailable";
    }
  }
  {
    const std::string cls =
        read_first_line("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size");
    if (!cls.empty()) {
      env.cache_line_bytes = std::stoull(cls);
    }
  }
#endif

  env.clock_overhead_ns = calibrate_clock_overhead_ns();
  return env;
}

std::string environment_to_json(const EnvironmentInfo& env) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  oss << "{\n";
  oss << "  \"timestamp_utc\": \"" << json_escape(env.timestamp_utc) << "\",\n";
  oss << "  \"cpu_model\": \"" << json_escape(env.cpu_model) << "\",\n";
  oss << "  \"architecture\": \"" << json_escape(env.architecture) << "\",\n";
  oss << "  \"logical_cores\": " << env.logical_cores << ",\n";
  oss << "  \"physical_cores\": " << env.physical_cores << ",\n";
  oss << "  \"perf_cores\": " << env.perf_cores << ",\n";
  oss << "  \"eff_cores\": " << env.eff_cores << ",\n";
  oss << "  \"l1d_cache_bytes\": " << env.l1d_cache_bytes << ",\n";
  oss << "  \"l2_cache_bytes\": " << env.l2_cache_bytes << ",\n";
  oss << "  \"l3_cache_bytes\": " << env.l3_cache_bytes << ",\n";
  oss << "  \"cache_line_bytes\": " << env.cache_line_bytes << ",\n";
  oss << "  \"os_name\": \"" << json_escape(env.os_name) << "\",\n";
  oss << "  \"kernel_release\": \"" << json_escape(env.kernel_release) << "\",\n";
  oss << "  \"compiler_id\": \"" << json_escape(env.compiler_id) << "\",\n";
  oss << "  \"compiler_version\": \"" << json_escape(env.compiler_version) << "\",\n";
  oss << "  \"build_type\": \"" << json_escape(env.build_type) << "\",\n";
  oss << "  \"cxx_flags\": \"" << json_escape(env.cxx_flags) << "\",\n";
  oss << "  \"march_native\": " << (env.march_native ? "true" : "false") << ",\n";
  oss << "  \"cpu_governor\": \"" << json_escape(env.cpu_governor) << "\",\n";
  oss << "  \"turbo_status\": \"" << json_escape(env.turbo_status) << "\",\n";
  oss << "  \"clock_overhead_ns\": " << env.clock_overhead_ns << "\n";
  oss << "}\n";
  return oss.str();
}

std::string environment_to_markdown(const EnvironmentInfo& env) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  oss << "| Field | Value |\n";
  oss << "|---|---|\n";
  oss << "| CPU / cores / topology | " << env.cpu_model << " (" << env.architecture << "), "
      << env.physical_cores << " physical / " << env.logical_cores << " logical";
  if (env.perf_cores > 0 || env.eff_cores > 0) {
    oss << " (" << env.perf_cores << "P + " << env.eff_cores << "E)";
  }
  oss << ", cache line " << env.cache_line_bytes << " B |\n";
  oss << "| Caches (L1d / L2 / L3) | " << (env.l1d_cache_bytes / 1024) << " KiB / "
      << (env.l2_cache_bytes / 1024) << " KiB / " << (env.l3_cache_bytes / 1024) << " KiB |\n";
  oss << "| OS / kernel | " << env.os_name << " (kernel " << env.kernel_release << ") |\n";
  oss << "| Compiler / version / flags | " << env.compiler_id << " " << env.compiler_version
      << " (" << env.build_type << ", flags: `" << env.cxx_flags
      << "`, `-march=native`: " << (env.march_native ? "ON" : "OFF") << ") |\n";
  oss << "| Governor / turbo | " << env.cpu_governor << " / " << env.turbo_status << " |\n";
  oss << "| Clock overhead (median) | " << env.clock_overhead_ns << " ns |\n";
  oss << "| Captured (UTC) | " << env.timestamp_utc << " |\n";
  return oss.str();
}

}  // namespace lpl
