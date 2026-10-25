/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Error.h>
#include <LibTest/TestCase.h>

namespace {

constexpr int invalid_argument = 22;

ErrorOr<int> parse_positive(StringView text)
{
    auto number = text.to_number<int>();
    if (!number.has_value())
        return Error::from_string_literal("not a number");
    if (number.value() <= 0)
        return Error::from_errno(invalid_argument);
    return number.value();
}

ErrorOr<int> parse_and_double(StringView text)
{
    int value = TRY(parse_positive(text));
    return value * 2;
}

ErrorOr<void> require(bool condition)
{
    if (!condition)
        return Error::from_errno(1);
    return {};
}

ErrorOr<int> chained(bool condition)
{
    TRY(require(condition));
    return TRY(parse_and_double(TRY(parse_positive("5"_sv)) == 5 ? "21"_sv : "x"_sv));
}

int s_live_objects = 0;

struct Tracked {
    Tracked() { ++s_live_objects; }
    Tracked(Tracked const&) { ++s_live_objects; }
    Tracked(Tracked&&) { ++s_live_objects; }
    ~Tracked() { --s_live_objects; }
};

}

TEST_CASE(value_and_error)
{
    auto good = parse_positive("12"_sv);
    EXPECT(!good.is_error());
    EXPECT_EQ(good.value(), 12);

    auto not_a_number = parse_positive("twelve"_sv);
    EXPECT(not_a_number.is_error());
    EXPECT_EQ(not_a_number.error().string_literal(), "not a number"_sv);
    EXPECT(!not_a_number.error().is_errno());

    auto negative = parse_positive("-3"_sv);
    EXPECT(negative.is_error());
    EXPECT_EQ(negative.error().code(), invalid_argument);
}

TEST_CASE(try_propagates)
{
    EXPECT_EQ(parse_and_double("21"_sv).value(), 42);
    EXPECT(parse_and_double("nope"_sv).is_error());

    EXPECT_EQ(chained(true).value(), 42);
    auto failed = chained(false);
    EXPECT(failed.is_error());
    EXPECT_EQ(failed.error(), Error::from_errno(1));
}

TEST_CASE(void_results)
{
    EXPECT(!require(true).is_error());
    EXPECT(require(false).is_error());
}

TEST_CASE(must)
{
    int value = MUST(parse_positive("8"_sv));
    EXPECT_EQ(value, 8);
}

TEST_CASE(object_lifetimes)
{
    {
        ErrorOr<Tracked> result = Tracked {};
        EXPECT_EQ(s_live_objects, 1);
        ErrorOr<Tracked> moved = move(result);
        EXPECT_EQ(s_live_objects, 2);
        ErrorOr<Tracked> error = Error::from_errno(5);
        EXPECT_EQ(s_live_objects, 2);
        error = move(moved);
        EXPECT_EQ(s_live_objects, 3);
    }
    EXPECT_EQ(s_live_objects, 0);
}
