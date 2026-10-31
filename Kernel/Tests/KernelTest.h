/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Debug/KernelLog.h>

// Tests that run inside the booted kernel, on real (virtual) hardware: the
// allocators, locks, timers and processors. Booting with "ktest" runs them
// all ("ktest=heap" runs those whose name starts with "heap"), reports on
// the serial console and powers the machine off.
//
//     KERNEL_TEST(heap, zeroed_allocations)
//     {
//         KTEST_EXPECT(kzalloc(64) != nullptr);
//     }
//
// Expectations may fail on any processor; the test keeps running.

namespace Kernel::Tests {

using TestFunction = void (*)();

class TestCase {
public:
    TestCase(char const* suite, char const* name, TestFunction);

    [[nodiscard]] char const* suite() const { return m_suite; }
    [[nodiscard]] char const* name() const { return m_name; }
    void run() const { m_function(); }
    [[nodiscard]] TestCase const* next() const { return m_next; }

    [[nodiscard]] static TestCase const* first();

private:
    char const* m_suite;
    char const* m_name;
    TestFunction m_function;
    TestCase const* m_next { nullptr };
};

[[gnu::cold]] void fail(char const* file, unsigned line, StringView message);

template<typename A, typename B>
[[gnu::cold]] void fail_equal(char const* file, unsigned line, char const* lhs_text, char const* rhs_text, A const& lhs, B const& rhs)
{
    FixedStringBuilder<256> message;
    format_to(message, "EXPECT_EQ({}, {}) failed: {} != {}", lhs_text, rhs_text, lhs, rhs);
    fail(file, line, message.view());
}

template<typename A, typename B>
void expect_equal(char const* file, unsigned line, char const* lhs_text, char const* rhs_text, A const& lhs, B const& rhs)
{
    if (!(lhs == rhs)) [[unlikely]]
        fail_equal(file, line, lhs_text, rhs_text, lhs, rhs);
}

// Runs the tests whose "suite.name" starts with `filter`, prints a summary
// ending in "ktest: PASS" or "ktest: FAIL", and powers off.
[[noreturn]] void run(StringView filter);

}

#define KERNEL_TEST(suite, name)                                                                                                            \
    static void kernel_test_##suite##_##name();                                                                                             \
    [[maybe_unused]] static ::Kernel::Tests::TestCase const kernel_test_case_##suite##_##name(#suite, #name, kernel_test_##suite##_##name); \
    static void kernel_test_##suite##_##name()

#define KTEST_EXPECT(expression)                                                                 \
    do {                                                                                         \
        if (!(expression)) [[unlikely]]                                                          \
            ::Kernel::Tests::fail(__FILE_NAME__, __LINE__, "EXPECT(" #expression ") failed"_sv); \
    } while (0)

#define KTEST_EXPECT_EQ(lhs, rhs) ::Kernel::Tests::expect_equal(__FILE_NAME__, __LINE__, #lhs, #rhs, (lhs), (rhs))
