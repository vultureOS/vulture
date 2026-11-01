/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibTest/TestCase.h>

namespace {

int add(int a, int b)
{
    return a + b;
}

struct Counted {
    static inline int alive = 0;
    Counted() { ++alive; }
    Counted(Counted const&) { ++alive; }
    Counted(Counted&&) { ++alive; }
    ~Counted() { --alive; }
};

}

TEST_CASE(function_calls_what_it_holds)
{
    Function<int(int, int)> function = add;
    EXPECT_EQ(function(2, 3), 5);
    int base = 10;
    function = [base](int a, int b) { return base + a * b; };
    EXPECT_EQ(function(2, 3), 16);
}

TEST_CASE(function_empty_and_moved)
{
    Function<void()> empty;
    EXPECT(!empty);
    int calls = 0;
    Function<void()> counter = [&calls] { ++calls; };
    EXPECT(static_cast<bool>(counter));
    Function<void()> moved = move(counter);
    EXPECT(!counter);
    moved();
    moved();
    EXPECT_EQ(calls, 2);
    moved = nullptr;
    EXPECT(!moved);
}

TEST_CASE(function_owns_its_captures)
{
    EXPECT_EQ(Counted::alive, 0);
    {
        Counted counted;
        Function<int()> function = [counted] { return Counted::alive; };
        EXPECT(function() >= 2);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST_CASE(function_takes_move_only_captures)
{
    auto owned = adopt_own(*new int(42));
    Function<int()> function = [owned = move(owned)] { return *owned; };
    EXPECT_EQ(function(), 42);
}
