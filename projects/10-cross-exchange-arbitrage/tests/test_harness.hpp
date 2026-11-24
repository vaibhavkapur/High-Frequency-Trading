#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace xarb::test {

template <typename T>
std::string fmt_val(const T& v) {
  std::ostringstream oss;
  if constexpr (std::is_enum_v<T>) {
    oss << static_cast<long long>(v);
  } else {
    oss << v;
  }
  return oss.str();
}

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failure_count() {
  static int c = 0;
  return c;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all(const char* suite_label) {
  int passed = 0;
  int failed = 0;
  for (const auto& tc : registry()) {
    const int before = failure_count();
    try {
      tc.fn();
    } catch (const std::exception& ex) {
      std::cerr << "[EXCEPTION] " << tc.name << ": " << ex.what() << "\n";
      failure_count() += 1;
    }
    if (failure_count() == before) {
      ++passed;
    } else {
      ++failed;
      std::cerr << "[FAIL] " << tc.name << "\n";
    }
  }
  std::cout << "[" << suite_label << "] passed=" << passed << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace xarb::test

#define XARB_TEST(name)                                                           \
  static void test_fn_##name();                                                   \
  static ::xarb::test::Registrar test_reg_##name(#name, test_fn_##name);          \
  static void test_fn_##name()

#define EXPECT_TRUE(cond)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_TRUE failed: " #cond "\n"; \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b)                                                                                       \
  do {                                                                                                        \
    const auto _va = (a);                                                                                     \
    const auto _vb = (b);                                                                                     \
    if (!(_va == _vb)) {                                                                                      \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_EQ failed: " #a " ("                              \
                << ::xarb::test::fmt_val(_va) << ") != " #b " (" << ::xarb::test::fmt_val(_vb) << ")\n";      \
      ::xarb::test::failure_count() += 1;                                                                     \
    }                                                                                                         \
  } while (0)

#define EXPECT_NE(a, b)                                                           \
  do {                                                                            \
    const auto _va = (a);                                                         \
    const auto _vb = (b);                                                         \
    if (_va == _vb) {                                                             \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_NE failed: " #a " == " #b "\n"; \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)

#define EXPECT_GT(a, b)                                                           \
  do {                                                                            \
    const auto _va = (a);                                                         \
    const auto _vb = (b);                                                         \
    if (!(_va > _vb)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_GT failed: " << _va << " <= " << _vb << "\n"; \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)

#define EXPECT_GE(a, b)                                                           \
  do {                                                                            \
    const auto _va = (a);                                                         \
    const auto _vb = (b);                                                         \
    if (!(_va >= _vb)) {                                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_GE failed: " << _va << " < " << _vb << "\n"; \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)

#define EXPECT_LT(a, b)                                                           \
  do {                                                                            \
    const auto _va = (a);                                                         \
    const auto _vb = (b);                                                         \
    if (!(_va < _vb)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_LT failed: " << _va << " >= " << _vb << "\n"; \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)

#define EXPECT_NEAR(a, b, tol)                                                    \
  do {                                                                            \
    const double _va = static_cast<double>(a);                                    \
    const double _vb = static_cast<double>(b);                                    \
    if (std::fabs(_va - _vb) > (tol)) {                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": EXPECT_NEAR failed: " << _va << " vs " << _vb \
                << " (tol=" << (tol) << ")\n";                                    \
      ::xarb::test::failure_count() += 1;                                         \
    }                                                                             \
  } while (0)
