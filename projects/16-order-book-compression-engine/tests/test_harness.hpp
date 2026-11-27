#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace obce_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline void register_test(const std::string& name, std::function<void()> fn) {
    registry().push_back(TestCase{name, std::move(fn)});
}

struct AssertionError : public std::runtime_error {
    explicit AssertionError(const std::string& msg) : std::runtime_error(msg) {}
};

#define OBCE_ASSERT_TRUE(cond)                                                     \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::ostringstream oss_;                                               \
            oss_ << __FILE__ << ":" << __LINE__ << ": Expected true: " << #cond;   \
            throw ::obce_test::AssertionError(oss_.str());                         \
        }                                                                          \
    } while (0)

#define OBCE_ASSERT_EQ(a, b)                                                       \
    do {                                                                           \
        const auto a_val_ = (a);                                                   \
        const auto b_val_ = (b);                                                   \
        if (!(a_val_ == b_val_)) {                                                 \
            std::ostringstream oss_;                                               \
            oss_ << __FILE__ << ":" << __LINE__ << ": Expected equality ("         \
                 << #a << " == " << #b << ")";                                     \
            throw ::obce_test::AssertionError(oss_.str());                         \
        }                                                                          \
    } while (0)

#define OBCE_ASSERT_NE(a, b)                                                       \
    do {                                                                           \
        const auto a_val_ = (a);                                                   \
        const auto b_val_ = (b);                                                   \
        if (a_val_ == b_val_) {                                                    \
            std::ostringstream oss_;                                               \
            oss_ << __FILE__ << ":" << __LINE__ << ": Expected inequality ("       \
                 << #a << " != " << #b << ")";                                     \
            throw ::obce_test::AssertionError(oss_.str());                         \
        }                                                                          \
    } while (0)

inline int run_all_tests(const std::string& suite_name) {
    int passed = 0;
    int failed = 0;
    std::cout << "[SUITE] " << suite_name << " (" << registry().size() << " tests)\n";
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
    std::cout << "[SUMMARY] " << suite_name << ": " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace obce_test
