// SPDX-License-Identifier: Apache-2.0
// A minimal test framework for x2_tests.exe. Write a test as
//     TEST(name_of_test) { CHECK(a == b); CHECK_EQ(a, b); }
// Every test runs once, in file order; a failing CHECK prints file:line and the test keeps going.
#pragma once
#include <stdio.h>

namespace t {

typedef void (*test_fn)();
struct TestCase { const char* name; test_fn fn; TestCase* next; };

void add(TestCase* tc);
void fail(const char* file, int line, const char* what);
void skip(const char* why);   // marks the running test as skipped (e.g. game files not installed)
bool slow_tests();             // x2_tests.exe --slow: also run the tests that take minutes

struct Register {
    Register(TestCase* tc) { add(tc); }
};

} // namespace t

#define TEST(name)                                                         \
    static void name();                                                    \
    static t::TestCase name##_case = { #name, name, nullptr };             \
    static t::Register name##_reg(&name##_case);                           \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) t::fail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        long long a_ = (long long)(actual), e_ = (long long)(expected);                    \
        if (a_ != e_) {                                                                    \
            char m_[256];                                                                  \
            snprintf(m_, sizeof(m_), "%s == %s (got %lld, expected %lld)", #actual, #expected, a_, e_); \
            t::fail(__FILE__, __LINE__, m_);                                               \
        }                                                                                  \
    } while (0)
