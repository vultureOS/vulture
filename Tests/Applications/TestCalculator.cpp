/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Calculator/Engine.h>
#include <LibTest/TestCase.h>

using Calculator::Engine;
using Calculator::Operation;

namespace {

// Keys, one character each.
void keys(Engine& engine, StringView text)
{
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9')
            engine.digit(c - '0');
        else if (c == '.')
            engine.decimal_point();
        else if (c == '+')
            engine.operation(Operation::Add);
        else if (c == '-')
            engine.operation(Operation::Subtract);
        else if (c == '*')
            engine.operation(Operation::Multiply);
        else if (c == '/')
            engine.operation(Operation::Divide);
        else if (c == '=')
            engine.equals();
        else if (c == '%')
            engine.percent();
        else if (c == 'n')
            engine.negate();
        else if (c == 'c')
            engine.clear();
        else if (c == 'b')
            engine.backspace();
    }
}

bool shows(StringView text, StringView expected)
{
    Engine engine;
    keys(engine, text);
    return engine.display().view() == expected;
}

}

TEST_CASE(it_starts_at_nothing)
{
    Engine engine;
    EXPECT(engine.display().view() == "0"_sv);
    EXPECT(engine.shows_all_clear());
}

TEST_CASE(typing_shows_what_was_typed)
{
    EXPECT(shows("123"_sv, "123"_sv));
    EXPECT(shows("007"_sv, "7"_sv));
    EXPECT(shows("1.50"_sv, "1.50"_sv));
    EXPECT(shows(".5"_sv, "0.5"_sv));
    EXPECT(shows("1..2"_sv, "1.2"_sv));
    EXPECT(shows("1234567"_sv, "1,234,567"_sv));
    EXPECT(shows("123456789012345"_sv, "123,456,789,012"_sv));
    EXPECT(shows("12b"_sv, "1"_sv));
    EXPECT(shows("1b"_sv, "0"_sv));
    EXPECT(shows("1.b"_sv, "1"_sv));
    EXPECT(shows("5n"_sv, "-5"_sv));
    EXPECT(shows("5nn"_sv, "5"_sv));
}

TEST_CASE(the_four_functions)
{
    EXPECT(shows("2+3="_sv, "5"_sv));
    EXPECT(shows("2-3="_sv, "-1"_sv));
    EXPECT(shows("6*7="_sv, "42"_sv));
    EXPECT(shows("1/4="_sv, "0.25"_sv));
    EXPECT(shows("1/3="_sv, "0.333333333333"_sv));
    EXPECT(shows("2/3="_sv, "0.666666666667"_sv));
}

TEST_CASE(multiplication_goes_first)
{
    EXPECT(shows("2+3*4="_sv, "14"_sv));
    EXPECT(shows("2*3+4="_sv, "10"_sv));
    EXPECT(shows("2+3*4-1="_sv, "13"_sv));
    EXPECT(shows("1+2*3/4="_sv, "2.5"_sv));
    // What is known so far shows as it goes.
    EXPECT(shows("2+3*"_sv, "3"_sv));
    EXPECT(shows("2*3+"_sv, "6"_sv));
    EXPECT(shows("2+3+"_sv, "5"_sv));
}

TEST_CASE(binary_fractions_do_not_show)
{
    EXPECT(shows("0.1+0.2="_sv, "0.3"_sv));
    EXPECT(shows("1.1*1.1="_sv, "1.21"_sv));
    EXPECT(shows("0.3-0.1="_sv, "0.2"_sv));
    EXPECT(shows("1/3*3="_sv, "1"_sv));
}

TEST_CASE(equals_again_repeats)
{
    EXPECT(shows("2+3=="_sv, "8"_sv));
    EXPECT(shows("2+3==="_sv, "11"_sv));
    EXPECT(shows("2*3=="_sv, "18"_sv));
    EXPECT(shows("2+="_sv, "4"_sv));
    // A new number after =: the sum is over.
    EXPECT(shows("2+3=7="_sv, "7"_sv));
}

TEST_CASE(a_second_operator_replaces_the_first)
{
    EXPECT(shows("2+*3="_sv, "6"_sv));
    EXPECT(shows("2*+3="_sv, "5"_sv));
}

TEST_CASE(percent_is_a_share_of_the_sum)
{
    EXPECT(shows("50%"_sv, "0.5"_sv));
    EXPECT(shows("100+10%="_sv, "110"_sv));
    EXPECT(shows("200-25%="_sv, "150"_sv));
    EXPECT(shows("200*10%="_sv, "20"_sv));
}

TEST_CASE(clearing_goes_one_step_at_a_time)
{
    Engine engine;
    keys(engine, "12+34"_sv);
    EXPECT(!engine.shows_all_clear());
    keys(engine, "c"_sv);
    EXPECT(engine.display().view() == "0"_sv);
    EXPECT(engine.shows_all_clear());
    // The 12 + is still waiting.
    keys(engine, "1="_sv);
    EXPECT(engine.display().view() == "13"_sv);
    keys(engine, "cc"_sv);
    EXPECT(engine.display().view() == "0"_sv);
    keys(engine, "5="_sv);
    EXPECT(engine.display().view() == "5"_sv);
}

TEST_CASE(dividing_by_zero_is_an_error_until_a_new_start)
{
    Engine engine;
    keys(engine, "5/0="_sv);
    EXPECT(engine.has_error());
    EXPECT(engine.display().view() == "Error"_sv);
    keys(engine, "+1="_sv);
    EXPECT(engine.has_error() == false || engine.display().view() == "Error"_sv);
    keys(engine, "c"_sv);
    EXPECT(!engine.has_error());
    keys(engine, "5/0=3"_sv);
    EXPECT(engine.display().view() == "3"_sv);
}

TEST_CASE(large_and_small_go_scientific)
{
    EXPECT(shows("999999999999*999999999999="_sv, "9.99999999998e23"_sv));
    EXPECT(shows("1/1000000="_sv, "0.000001"_sv));
    EXPECT(shows("1/10000000="_sv, "1e-7"_sv));
    EXPECT(shows("100000000000*10="_sv, "1e12"_sv));
    EXPECT(shows("99999999999*1="_sv, "99,999,999,999"_sv));
    EXPECT(shows("5n*2="_sv, "-10"_sv));
    EXPECT(shows("1000n*1000="_sv, "-1,000,000"_sv));
}

TEST_CASE(numbers_from_elsewhere)
{
    Engine engine;
    EXPECT(engine.set_from_text("1,234.5"_sv));
    EXPECT(engine.plain_display().view() == "1234.5"_sv);
    keys(engine, "+1="_sv);
    EXPECT(engine.plain_display().view() == "1235.5"_sv);
    EXPECT(!engine.set_from_text("hello"_sv));
    EXPECT(engine.plain_display().view() == "1235.5"_sv);
    EXPECT(engine.set_from_text("-2"_sv));
    keys(engine, "*3="_sv);
    EXPECT(engine.plain_display().view() == "-6"_sv);
}
