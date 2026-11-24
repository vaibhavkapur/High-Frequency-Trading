// Small helpers shared by the benchmarks. No external benchmark library is
// used so the numbers depend only on what is in this repository.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

// Keeps a value alive past the optimiser without a memory barrier per call.
template <typename T>
inline void do_not_optimize(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct Summary {
  std::size_t count = 0;
  double mean = 0;
  std::uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

// Exact percentiles by sorting (nearest-rank). Sorts `v` in place.
inline Summary summarize(std::vector<std::uint64_t>& v) {
  Summary s;
  s.count = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const auto at = [&](double q) {
    std::size_t rank = static_cast<std::size_t>(q * static_cast<double>(v.size()) + 0.999999);
    if (rank < 1) rank = 1;
    if (rank > v.size()) rank = v.size();
    return v[rank - 1];
  };
  long double sum = 0;
  for (std::uint64_t x : v) sum += static_cast<long double>(x);
  s.mean = static_cast<double>(sum / static_cast<long double>(v.size()));
  s.min = v.front();
  s.p50 = at(0.50);
  s.p90 = at(0.90);
  s.p99 = at(0.99);
  s.p999 = at(0.999);
  s.max = v.back();
  return s;
}

inline std::string compiler_string() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#else
  return "unknown";
#endif
}

inline const char* build_type() {
#ifdef NDEBUG
  return "release (NDEBUG)";
#else
  return "debug (assertions on)";
#endif
}

}  // namespace bench
