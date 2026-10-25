/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>

// A minimal unit-test framework. Test cases register themselves at static
// initialization; failed expectations are reported and the test keeps going.
//
//     TEST_CASE(adds_numbers)
//     {
//         EXPECT_EQ(1 + 1, 2);
//     }

namespace Test {

using TestFunction = void (*)();

class TestCase {
public:
    TestCase(char const* name, TestFunction function);

    [[nodiscard]] char const* name() const { return m_name; }
    void run() const { m_function(); }

    [[nodiscard]] TestCase const* next() const { return m_next; }
    [[nodiscard]] static TestCase const* first();

private:
    char const* m_name;
    TestFunction m_function;
    TestCase const* m_next { nullptr };
};

void print(StringView);
[[gnu::cold]] void fail(char const* file, unsigned line, StringView message);

template<typename T>
void format_value(FormatSink& sink, T const& value)
{
    if constexpr (Base::HasFormatter<T>)
        format_to(sink, "{}", value);
    else
        sink.write("<unprintable>"_sv);
}

template<typename A, typename B>
[[gnu::cold]] void fail_equal(char const* file, unsigned line, char const* lhs_text, char const* rhs_text, A const& lhs, B const& rhs)
{
    FixedStringBuilder<1024> message;
    format_to(message, "EXPECT_EQ({}, {}) failed: ", lhs_text, rhs_text);
    format_value(message, lhs);
    message.append(" != "_sv);
    format_value(message, rhs);
    fail(file, line, message.view());
}

// Compares inside a single call so temporaries in either operand (such as
// `make_result().value()`) live until the comparison is done.
template<typename A, typename B>
void expect_equal(char const* file, unsigned line, char const* lhs_text, char const* rhs_text, A const& lhs, B const& rhs)
{
    if (!(lhs == rhs)) [[unlikely]]
        fail_equal(file, line, lhs_text, rhs_text, lhs, rhs);
}

}

#define TEST_CASE(name)                                                                           \
    static void test_function_##name();                                                           \
    [[maybe_unused]] static ::Test::TestCase const test_case_##name(#name, test_function_##name); \
    static void test_function_##name()

#define EXPECT(expression)                                                           \
    do {                                                                             \
        if (!(expression))                                                           \
            ::Test::fail(__FILE_NAME__, __LINE__, "EXPECT(" #expression ") failed"); \
    } while (0)

#define EXPECT_EQ(lhs, rhs) ::Test::expect_equal(__FILE_NAME__, __LINE__, #lhs, #rhs, (lhs), (rhs))
