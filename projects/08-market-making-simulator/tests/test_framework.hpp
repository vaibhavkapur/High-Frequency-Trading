#pragma once

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mmsim_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{name, std::move(fn)});
    }
};

struct AssertionFailure : public std::exception {
    std::string msg;
    explicit AssertionFailure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

inline int run_all_tests() {
    int passed = 0;
    int failed = 0;
    for (const auto& tc : registry()) {
        try {
            tc.fn();
            ++passed;
            std::cout << "[  OK  ] " << tc.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[ FAIL ] " << tc.name << ": " << e.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "[ FAIL ] " << tc.name << ": unknown exception\n";
        }
    }
    std::cout << "Summary: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace mmsim_test

#define TEST(Name)                                                                                \
    static void test_fn_##Name();                                                                 \
    static mmsim_test::Registrar test_reg_##Name(#Name, test_fn_##Name);                          \
    static void test_fn_##Name()

#define EXPECT_TRUE(cond)                                                                         \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::ostringstream oss_;                                                              \
            oss_ << __FILE__ << ":" << __LINE__ << " EXPECT_TRUE failed: " << #cond;              \
            throw mmsim_test::AssertionFailure(oss_.str());                                       \
        }                                                                                         \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b)                                                                           \
    do {                                                                                          \
        const auto a_ = (a);                                                                      \
        const auto b_ = (b);                                                                      \
        if (!(a_ == b_)) {                                                                        \
            std::ostringstream oss_;                                                              \
            oss_ << __FILE__ << ":" << __LINE__ << " EXPECT_EQ failed: " << #a << " (" << a_      \
                 << ") != " << #b << " (" << b_ << ")";                                           \
            throw mmsim_test::AssertionFailure(oss_.str());                                       \
        }                                                                                         \
    } while (0)

#define EXPECT_LE(a, b)                                                                           \
    do {                                                                                          \
        const auto a_ = (a);                                                                      \
        const auto b_ = (b);                                                                      \
        if (!(a_ <= b_)) {                                                                        \
            std::ostringstream oss_;                                                              \
            oss_ << __FILE__ << ":" << __LINE__ << " EXPECT_LE failed: " << #a << " (" << a_      \
                 << ") > " << #b << " (" << b_ << ")";                                            \
            throw mmsim_test::AssertionFailure(oss_.str());                                       \
        }                                                                                         \
    } while (0)

#define EXPECT_GE(a, b)                                                                           \
    do {                                                                                          \
        const auto a_ = (a);                                                                      \
        const auto b_ = (b);                                                                      \
        if (!(a_ >= b_)) {                                                                        \
            std::ostringstream oss_;                                                              \
            oss_ << __FILE__ << ":" << __LINE__ << " EXPECT_GE failed: " << #a << " (" << a_      \
                 << ") < " << #b << " (" << b_ << ")";                                            \
            throw mmsim_test::AssertionFailure(oss_.str());                                       \
        }                                                                                         \
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                                                    \
    do {                                                                                          \
        const double a_ = static_cast<double>(a);                                                 \
        const double b_ = static_cast<double>(b);                                                 \
        const double t_ = static_cast<double>(tol);                                               \
        if (std::isnan(a_) || std::isnan(b_) || std::abs(a_ - b_) > t_) {                         \
            std::ostringstream oss_;                                                              \
            oss_ << __FILE__ << ":" << __LINE__ << " EXPECT_NEAR failed: |" << a_ << " - " << b_  \
                 << "| = " << std::abs(a_ - b_) << " > " << t_;                                   \
            throw mmsim_test::AssertionFailure(oss_.str());                                       \
        }                                                                                         \
    } while (0)

#define MMSIM_TEST_MAIN()                                                                         \
    int main() { return mmsim_test::run_all_tests(); }
