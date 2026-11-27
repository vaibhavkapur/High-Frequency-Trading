#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kbnl::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline void register_test(const std::string& name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
}

struct AssertionFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define KBNL_EXPECT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream _oss; \
            _oss << __FILE__ << ":" << __LINE__ << ": expected true: " << #cond; \
            throw kbnl::test::AssertionFailure(_oss.str()); \
        } \
    } while (0)

#define KBNL_EXPECT_FALSE(cond) KBNL_EXPECT_TRUE(!(cond))

#define KBNL_EXPECT_EQ(actual, expected) \
    do { \
        const auto _a = (actual); \
        const auto _e = (expected); \
        if (!(_a == _e)) { \
            std::ostringstream _oss; \
            _oss << __FILE__ << ":" << __LINE__ << ": expected " << #actual \
                 << " == " << #expected << " (actual=" << _a << ", expected=" << _e << ")"; \
            throw kbnl::test::AssertionFailure(_oss.str()); \
        } \
    } while (0)

#define KBNL_EXPECT_NEAR(actual, expected, tol) \
    do { \
        const double _a = static_cast<double>(actual); \
        const double _e = static_cast<double>(expected); \
        if (std::fabs(_a - _e) > (tol)) { \
            std::ostringstream _oss; \
            _oss << __FILE__ << ":" << __LINE__ << ": expected " << #actual \
                 << " near " << _e << " +/- " << (tol) << " (actual=" << _a << ")"; \
            throw kbnl::test::AssertionFailure(_oss.str()); \
        } \
    } while (0)

#define KBNL_TEST(name) \
    static void test_fn_##name(); \
    struct test_reg_##name { \
        test_reg_##name() { kbnl::test::register_test(#name, test_fn_##name); } \
    } test_reg_inst_##name; \
    static void test_fn_##name()

inline int run_all_tests(const char* suite_name) {
    int passed = 0;
    int failed = 0;
    std::cout << "=== Running suite: " << suite_name << " (" << registry().size() << " tests) ===\n";
    for (const auto& tc : registry()) {
        try {
            tc.fn();
            ++passed;
            std::cout << "  [PASS] " << tc.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "  [FAIL] " << tc.name << " -> " << ex.what() << "\n";
        }
    }
    std::cout << "=== Suite " << suite_name << ": " << passed << " passed, " << failed << " failed ===\n";
    return failed == 0 ? 0 : 1;
}

} // namespace kbnl::test
