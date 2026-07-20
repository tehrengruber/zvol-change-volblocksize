// Tiny test framework: register tests with TEST(name), assert with EXPECT_*.
// Each test is run as its own process (one CTest case) via tests/testing.cpp.
#pragma once

#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace testing {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using TestFn = std::function<void()>;
std::map<std::string, TestFn>& registry();

struct Registrar {
    Registrar(const std::string& name, TestFn fn);
};

extern std::string g_pool;  // pool for the current test (set by main)
extern std::string g_tool;  // path to the zvol-change-volblocksize binary

}  // namespace testing

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::testing::Registrar reg_##name(#name, name);                 \
    static void name()

#define EXPECT_TRUE(cond)                                                \
    do {                                                                 \
        if (!(cond))                                                     \
            throw ::testing::TestFailure(std::string(__FILE__ ":") +     \
                                         std::to_string(__LINE__) +      \
                                         ": EXPECT_TRUE(" #cond ")");    \
    } while (0)

#define EXPECT_EQ(a, b)                                                  \
    do {                                                                 \
        auto _va = (a);                                                  \
        auto _vb = (b);                                                  \
        if (!(_va == _vb)) {                                             \
            std::ostringstream _o;                                       \
            _o << __FILE__ << ":" << __LINE__ << ": EXPECT_EQ(" #a ", " #b \
                  ") -> [" << _va << "] != [" << _vb << "]";             \
            throw ::testing::TestFailure(_o.str());                      \
        }                                                                \
    } while (0)

#define EXPECT_NE(a, b)                                                  \
    do {                                                                 \
        auto _va = (a);                                                  \
        auto _vb = (b);                                                  \
        if (_va == _vb) {                                                \
            std::ostringstream _o;                                       \
            _o << __FILE__ << ":" << __LINE__ << ": EXPECT_NE(" #a ", " #b \
                  ") -> both [" << _va << "]";                           \
            throw ::testing::TestFailure(_o.str());                      \
        }                                                                \
    } while (0)
