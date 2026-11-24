#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mdf_test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& current_failures() {
  static int fails = 0;
  return fails;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back(TestCase{name, std::move(fn)});
  }
};

inline int run_all() {
  int failed_tests = 0;
  for (const auto& tc : registry()) {
    current_failures() = 0;
    try {
      tc.fn();
    } catch (const std::exception& ex) {
      ++current_failures();
      std::cerr << "  [EXCEPTION] " << tc.name << ": " << ex.what() << "\n";
    } catch (...) {
      ++current_failures();
      std::cerr << "  [EXCEPTION] " << tc.name << ": unknown exception\n";
    }
    if (current_failures() == 0) {
      std::cout << "[  PASS  ] " << tc.name << "\n";
    } else {
      ++failed_tests;
      std::cout << "[  FAIL  ] " << tc.name << " (" << current_failures() << " assertion(s))\n";
    }
  }
  std::cout << "Summary: " << (registry().size() - static_cast<std::size_t>(failed_tests)) << "/"
            << registry().size() << " passed.\n";
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace mdf_test

#define MDF_TEST(name)                                                              \
  static void mdf_test_fn_##name();                                                 \
  static ::mdf_test::Registrar mdf_test_reg_##name(#name, mdf_test_fn_##name);      \
  static void mdf_test_fn_##name()

#define EXPECT_TRUE(cond)                                                           \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      ++::mdf_test::current_failures();                                             \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << " EXPECT_TRUE failed: "   \
                << #cond << "\n";                                                   \
    }                                                                               \
  } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(lhs, rhs)                                                         \
  do {                                                                              \
    const auto _l = (lhs);                                                          \
    const auto _r = (rhs);                                                          \
    if (!(_l == _r)) {                                                              \
      ++::mdf_test::current_failures();                                             \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << " EXPECT_EQ failed: "     \
                << #lhs << " != " << #rhs << "\n";                                  \
    }                                                                               \
  } while (0)

#define EXPECT_NE(lhs, rhs)                                                         \
  do {                                                                              \
    const auto _l = (lhs);                                                          \
    const auto _r = (rhs);                                                          \
    if (_l == _r) {                                                                 \
      ++::mdf_test::current_failures();                                             \
      std::cerr << "  " << __FILE__ << ":" << __LINE__ << " EXPECT_NE failed: "     \
                << #lhs << " == " << #rhs << "\n";                                  \
    }                                                                               \
  } while (0)

#define EXPECT_GT(lhs, rhs) EXPECT_TRUE((lhs) > (rhs))
#define EXPECT_GE(lhs, rhs) EXPECT_TRUE((lhs) >= (rhs))
#define EXPECT_LT(lhs, rhs) EXPECT_TRUE((lhs) < (rhs))
#define EXPECT_LE(lhs, rhs) EXPECT_TRUE((lhs) <= (rhs))

#define MDF_TEST_MAIN() \
  int main() { return ::mdf_test::run_all(); }
