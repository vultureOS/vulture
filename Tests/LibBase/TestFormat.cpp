/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Error.h>
#include <LibBase/Format.h>
#include <LibTest/TestCase.h>

#define EXPECT_FORMAT(expected, ...)                      \
    do {                                                  \
        FixedStringBuilder<256> builder_;                 \
        format_to(builder_, __VA_ARGS__);                 \
        EXPECT_EQ(builder_.view(), StringView(expected)); \
    } while (0)

TEST_CASE(plain_text_and_escapes)
{
    EXPECT_FORMAT("", "");
    EXPECT_FORMAT("hello", "hello");
    EXPECT_FORMAT("{}", "{{}}");
    EXPECT_FORMAT("{1}", "{{{}}}", 1);
    EXPECT_FORMAT("a}b{c", "a}}b{{c");
}

TEST_CASE(integers)
{
    EXPECT_FORMAT("42", "{}", 42);
    EXPECT_FORMAT("-7", "{}", -7);
    EXPECT_FORMAT("0", "{}", 0u);
    EXPECT_FORMAT("255", "{}", u8(255));
    EXPECT_FORMAT("-128", "{}", i8(-128));
    EXPECT_FORMAT("18446744073709551615", "{}", NumericLimits<u64>::max());
    EXPECT_FORMAT("-9223372036854775808", "{}", NumericLimits<i64>::min());
    EXPECT_FORMAT("+5", "{:+}", 5);
    EXPECT_FORMAT("-5", "{:+}", -5);
}

TEST_CASE(bases_and_prefixes)
{
    EXPECT_FORMAT("ff", "{:x}", 255);
    EXPECT_FORMAT("FF", "{:X}", 255);
    EXPECT_FORMAT("0xff", "{:#x}", 255);
    EXPECT_FORMAT("0XFF", "{:#X}", 255);
    EXPECT_FORMAT("101", "{:b}", 5);
    EXPECT_FORMAT("0b101", "{:#b}", 5);
    EXPECT_FORMAT("17", "{:o}", 15);
    EXPECT_FORMAT("0o17", "{:#o}", 15);
    EXPECT_FORMAT("A", "{:c}", 65);
}

TEST_CASE(width_alignment_and_padding)
{
    EXPECT_FORMAT("    42", "{:6}", 42);
    EXPECT_FORMAT("42    ", "{:<6}", 42);
    EXPECT_FORMAT("  42  ", "{:^6}", 42);
    EXPECT_FORMAT("0000beef", "{:08x}", 0xbeef);
    EXPECT_FORMAT("0x0000beef", "{:#010x}", 0xbeef);
    EXPECT_FORMAT("-0042", "{:05}", -42);
    EXPECT_FORMAT("ab    |", "{:6}|", "ab");
    EXPECT_FORMAT("    ab", "{:>6}", "ab");
    EXPECT_FORMAT("**ab***", "{:*^7}", "ab");
    EXPECT_FORMAT("toolong", "{:3}", "toolong");
}

TEST_CASE(other_types)
{
    EXPECT_FORMAT("true false", "{} {}", true, false);
    EXPECT_FORMAT("c", "{}", 'c');
    EXPECT_FORMAT("99", "{:d}", 'c');
    EXPECT_FORMAT("view", "{}", "view"_sv);
    EXPECT_FORMAT("(null)", "{}", static_cast<char const*>(nullptr));
    EXPECT_FORMAT("nullptr", "{}", nullptr);
    EXPECT_FORMAT("0x0000000000001000", "{}", reinterpret_cast<void*>(0x1000));
    EXPECT_FORMAT("None", "{}", Optional<int> {});
    EXPECT_FORMAT("3", "{}", Optional<int> { 3 });
    EXPECT_FORMAT("errno 22", "{}", Error::from_errno(22));
    EXPECT_FORMAT("disk full", "{}", Error::from_string_literal("disk full"));
}

enum class Color : u8 {
    Red = 3,
};

TEST_CASE(enums_and_mixed_arguments)
{
    EXPECT_FORMAT("3", "{}", Color::Red);
    EXPECT_FORMAT("vultureOS 0.1 on aarch64 (8 CPUs)", "{} {}.{} on {} ({} CPUs)", "vultureOS"_sv, 0, 1, "aarch64", 8u);
}

TEST_CASE(char_arrays_stop_at_null)
{
    char buffer[16] { 'h', 'i', '\0', 'x' };
    EXPECT_FORMAT("hi", "{}", buffer);
}

TEST_CASE(fixed_builder_truncates)
{
    FixedStringBuilder<4> builder;
    format_to(builder, "{}", "abcdef");
    EXPECT_EQ(builder.view(), "abcd"_sv);
    EXPECT(builder.is_truncated());
    EXPECT_EQ(builder.null_terminated()[4], '\0');

    builder.clear();
    builder.append_formatted("{}", 12);
    EXPECT_EQ(builder.view(), "12"_sv);
    EXPECT(!builder.is_truncated());
}
