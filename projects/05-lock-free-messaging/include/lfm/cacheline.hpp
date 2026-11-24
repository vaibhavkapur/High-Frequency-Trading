#pragma once

#include <cstddef>

namespace lfm {

// Destructive-interference granularity used to separate producer-owned and
// consumer-owned state. std::hardware_destructive_interference_size is not
// used because it is an ABI hazard (GCC warns on it) and is not reliable
// across standard libraries. Override with -DLFM_CACHE_LINE_SIZE=<n>.
#if defined(LFM_CACHE_LINE_SIZE)
inline constexpr std::size_t kCacheLine = LFM_CACHE_LINE_SIZE;
#elif defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;  // Apple Silicon: 128-byte lines
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

}  // namespace lfm
