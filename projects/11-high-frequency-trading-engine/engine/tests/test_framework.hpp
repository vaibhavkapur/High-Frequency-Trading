#pragma once

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace hft::test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back(TestCase{name, std::move(fn)});
  }
};

inline int run_all(const char* suite_name) {
  int passed = 0;
  int failed = 0;
  for (const auto& tc : registry()) {
    const int before = failure_count();
    tc.fn();
    if (failure_count() == before) {
      std::cout << "[  PASS  ] " << suite_name << "." << tc.name << "\n";
      ++passed;
    } else {
      std::cerr << "[  FAIL  ] " << suite_name << "." << tc.name << "\n";
      ++failed;
    }
  }
  std::cout << suite_name << ": " << passed << "/" << (passed + failed) << " passed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace hft::test

#define HFT_TEST(name)                                                       \
  static void hft_test_fn_##name();                                          \
  static ::hft::test::Registrar hft_reg_##name(#name, hft_test_fn_##name);   \
  static void hft_test_fn_##name()

#define HFT_EXPECT_TRUE(cond)                                                \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "  " << __FILE__ << ":" << __LINE__                       \
                << " EXPECT_TRUE failed: " #cond << "\n";                    \
      ++::hft::test::failure_count();                                        \
    }                                                                        \
  } while (false)

#define HFT_EXPECT_EQ(a, b)                                                  \
  do {                                                                       \
    const auto& _va = (a);                                                   \
    const auto& _vb = (b);                                                   \
    if (!(_va == _vb)) {                                                     \
      std::cerr << "  " << __FILE__ << ":" << __LINE__                       \
                << " EXPECT_EQ failed: " #a " != " #b << "\n";               \
      ++::hft::test::failure_count();                                        \
    }                                                                        \
  } while (false)
