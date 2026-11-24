#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Minimal dependency-free test support: each test file is its own executable
// registered with CTest; a failed CHECK aborts with file/line.
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                                 \
    }                                                                               \
  } while (0)

#define RUN(test)                 \
  do {                            \
    std::printf("  %s\n", #test); \
    test();                       \
  } while (0)

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define LFM_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define LFM_TEST_SANITIZED 1
#endif

namespace testutil {

// Iteration count for stress tests: LFM_STRESS_ITERS overrides; sanitizer
// builds default to a tenth because they run 5-20x slower.
inline std::uint64_t stress_iters(std::uint64_t normal) {
  if (const char* env = std::getenv("LFM_STRESS_ITERS")) {
    return std::strtoull(env, nullptr, 10);
  }
#if defined(LFM_TEST_SANITIZED)
  return normal / 10;
#else
  return normal;
#endif
}

// splitmix64: small deterministic PRNG so failures reproduce from a seed.
struct Rng {
  std::uint64_t state;
  std::uint64_t next() {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
};

// Element type that counts live instances, to catch leaks and double destroys.
struct Tracked {
  static inline std::int64_t live = 0;
  std::uint64_t value = 0;

  Tracked() noexcept { ++live; }
  explicit Tracked(std::uint64_t v) noexcept : value(v) { ++live; }
  Tracked(const Tracked& o) noexcept : value(o.value) { ++live; }
  Tracked(Tracked&& o) noexcept : value(o.value) { ++live; }
  Tracked& operator=(const Tracked&) noexcept = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --live; }
};

}  // namespace testutil
