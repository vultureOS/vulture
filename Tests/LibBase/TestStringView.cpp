/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/StringView.h>
#include <LibTest/TestCase.h>

TEST_CASE(construction_and_length)
{
    StringView empty;
    EXPECT(empty.is_empty());
    EXPECT_EQ(empty.length(), 0u);

    EXPECT_EQ("hello"_sv.length(), 5u);
    EXPECT_EQ(StringView("abc").length(), 3u);
    EXPECT_EQ(StringView(static_cast<char const*>(nullptr)).length(), 0u);

    static_assert("abc"_sv.length() == 3);
    static_assert(StringView("vulture").length() == 7);
}

TEST_CASE(comparison)
{
    EXPECT("abc"_sv == "abc"_sv);
    EXPECT(!("abc"_sv == "abd"_sv));
    EXPECT(!("abc"_sv == "ab"_sv));
    EXPECT("abc"_sv == "abc");
    EXPECT("abc"_sv.compare("abd"_sv) < 0);
    EXPECT("b"_sv.compare("abc"_sv) > 0);
    EXPECT("ab"_sv.compare("abc"_sv) < 0);
    EXPECT_EQ("same"_sv.compare("same"_sv), 0);
    EXPECT("ABC"_sv.equals_ignoring_ascii_case("abc"_sv));
    EXPECT(!"ABD"_sv.equals_ignoring_ascii_case("abc"_sv));
}

TEST_CASE(substrings)
{
    auto name = "vultureOS"_sv;
    EXPECT_EQ(name.substring_view(0, 7), "vulture"_sv);
    EXPECT_EQ(name.substring_view(7), "OS"_sv);
    EXPECT_EQ(name.substring_view(9), ""_sv);
    EXPECT(name.starts_with("vul"_sv));
    EXPECT(name.starts_with('v'));
    EXPECT(!name.starts_with("OS"_sv));
    EXPECT(name.ends_with("OS"_sv));
    EXPECT(name.ends_with('S'));
    EXPECT(!name.ends_with("vulture"_sv));
}

TEST_CASE(find)
{
    auto path = "a/b/c"_sv;
    EXPECT_EQ(path.find('/'), 1u);
    EXPECT_EQ(path.find('/', 2), 3u);
    EXPECT_EQ(path.find_last('/'), 3u);
    EXPECT(!path.find('x').has_value());
    EXPECT_EQ(path.find("b/c"_sv), 2u);
    EXPECT(!path.find("c/d"_sv).has_value());
    EXPECT(path.contains("/c"_sv));
    EXPECT(path.contains('b'));
    EXPECT(!path.contains('z'));

    static_assert("key=value"_sv.find('=').value() == 3);
}

TEST_CASE(trim_whitespace)
{
    EXPECT_EQ("  padded \t\n"_sv.trim_whitespace(), "padded"_sv);
    EXPECT_EQ("   "_sv.trim_whitespace(), ""_sv);
    EXPECT_EQ("clean"_sv.trim_whitespace(), "clean"_sv);
}

TEST_CASE(to_number)
{
    EXPECT_EQ("123"_sv.to_number<int>(), 123);
    EXPECT_EQ("+7"_sv.to_number<int>(), 7);
    EXPECT_EQ("-42"_sv.to_number<i32>(), -42);
    EXPECT_EQ("255"_sv.to_number<u8>(), u8(255));
    EXPECT_EQ("-128"_sv.to_number<i8>(), i8(-128));
    EXPECT_EQ("18446744073709551615"_sv.to_number<u64>(), NumericLimits<u64>::max());
    EXPECT_EQ("-9223372036854775808"_sv.to_number<i64>(), NumericLimits<i64>::min());

    EXPECT(!""_sv.to_number<int>().has_value());
    EXPECT(!"-"_sv.to_number<int>().has_value());
    EXPECT(!"12a"_sv.to_number<int>().has_value());
    EXPECT(!"256"_sv.to_number<u8>().has_value());
    EXPECT(!"-129"_sv.to_number<i8>().has_value());
    EXPECT(!"-1"_sv.to_number<u32>().has_value());
    EXPECT(!"18446744073709551616"_sv.to_number<u64>().has_value());
}

TEST_CASE(split)
{
    size_t count = 0;
    "usr//local/bin"_sv.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView piece) {
        EXPECT(!piece.is_empty());
        ++count;
    });
    EXPECT_EQ(count, 3u);

    count = 0;
    "usr//local/bin"_sv.for_each_split_view('/', SplitBehavior::KeepEmpty, [&](StringView) { ++count; });
    EXPECT_EQ(count, 4u);

    count = 0;
    ""_sv.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView) { ++count; });
    EXPECT_EQ(count, 0u);
}
