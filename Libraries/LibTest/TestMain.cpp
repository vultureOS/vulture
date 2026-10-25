/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibTest/TestCase.h>

namespace Test {

// Constant-initialized, so they are ready before any TestCase registers.
static TestCase const* s_first_test = nullptr;
static TestCase* s_last_test = nullptr;
static bool s_current_test_failed = false;

TestCase::TestCase(char const* name, TestFunction function)
    : m_name(name)
    , m_function(function)
{
    if (s_last_test)
        s_last_test->m_next = this;
    else
        s_first_test = this;
    s_last_test = this;
}

TestCase const* TestCase::first()
{
    return s_first_test;
}

void fail(char const* file, unsigned line, StringView message)
{
    s_current_test_failed = true;
    FixedStringBuilder<1280> report;
    format_to(report, "    {}:{}: {}\n", file, line, message);
    print(report.view());
}

}

int main()
{
    size_t passed = 0;
    size_t failed = 0;

    for (auto const* test = Test::TestCase::first(); test; test = test->next()) {
        Test::s_current_test_failed = false;
        test->run();

        FixedStringBuilder<256> line;
        format_to(line, "[{}] {}\n", Test::s_current_test_failed ? "FAIL" : "PASS", test->name());
        Test::print(line.view());

        if (Test::s_current_test_failed)
            ++failed;
        else
            ++passed;
    }

    FixedStringBuilder<128> summary;
    format_to(summary, "\n{} passed, {} failed\n", passed, failed);
    Test::print(summary.view());
    return failed == 0 ? 0 : 1;
}
