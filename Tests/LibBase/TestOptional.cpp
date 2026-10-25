/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Optional.h>
#include <LibTest/TestCase.h>

namespace {

int s_live_objects = 0;

struct Tracked {
    Tracked() { ++s_live_objects; }
    Tracked(Tracked const&) { ++s_live_objects; }
    Tracked(Tracked&&) { ++s_live_objects; }
    ~Tracked() { --s_live_objects; }
    Tracked& operator=(Tracked const&) = default;
    Tracked& operator=(Tracked&&) = default;
};

}

TEST_CASE(empty_and_value)
{
    Optional<int> number;
    EXPECT(!number.has_value());
    EXPECT_EQ(number.value_or(7), 7);

    number = 5;
    EXPECT(number.has_value());
    EXPECT_EQ(number.value(), 5);
    EXPECT_EQ(*number, 5);

    number.clear();
    EXPECT(!number.has_value());
    EXPECT(number == NullOpt);
}

TEST_CASE(comparison)
{
    Optional<int> a = 3;
    Optional<int> b = 3;
    Optional<int> empty;
    EXPECT(a == b);
    EXPECT(a == 3);
    EXPECT(!(a == 4));
    EXPECT(!(a == empty));
    EXPECT(empty == Optional<int> {});
}

TEST_CASE(trivially_copyable_is_trivial)
{
    static_assert(Base::IsTriviallyCopyable<Optional<int>>);
    static_assert(Base::IsTriviallyDestructible<Optional<int>>);
    static_assert(!Base::IsTriviallyDestructible<Optional<Tracked>>);

    constexpr Optional<int> constant = 42;
    static_assert(constant.has_value() && constant.value() == 42);
}

TEST_CASE(object_lifetimes)
{
    {
        Optional<Tracked> first;
        EXPECT_EQ(s_live_objects, 0);
        first.emplace();
        EXPECT_EQ(s_live_objects, 1);

        Optional<Tracked> copy = first;
        EXPECT_EQ(s_live_objects, 2);

        Optional<Tracked> moved = move(copy);
        EXPECT_EQ(s_live_objects, 3);

        first.clear();
        EXPECT_EQ(s_live_objects, 2);

        first = moved;
        EXPECT_EQ(s_live_objects, 3);
    }
    EXPECT_EQ(s_live_objects, 0);
}

TEST_CASE(release_value)
{
    Optional<int> number = 9;
    EXPECT_EQ(number.release_value(), 9);
    EXPECT(!number.has_value());

    {
        Optional<Tracked> tracked;
        tracked.emplace();
        Tracked released = tracked.release_value();
        EXPECT(!tracked.has_value());
        EXPECT_EQ(s_live_objects, 1);
    }
    EXPECT_EQ(s_live_objects, 0);
}
