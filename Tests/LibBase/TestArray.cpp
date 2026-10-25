/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Array.h>
#include <LibTest/TestCase.h>

TEST_CASE(aggregate_and_deduction)
{
    Array<int, 3> explicit_array { 1, 2, 3 };
    Array deduced { 4, 5, 6, 7 };
    static_assert(decltype(deduced)::size() == 4);

    EXPECT_EQ(explicit_array.size(), 3u);
    EXPECT_EQ(explicit_array[2], 3);
    EXPECT_EQ(deduced[0], 4);
}

TEST_CASE(span_and_fill)
{
    Array<u8, 4> bytes {};
    bytes.fill(0xab);
    Span<u8> span = bytes;
    EXPECT_EQ(span.size(), 4u);
    EXPECT_EQ(span[3], 0xab);

    int total = 0;
    for (auto byte : bytes)
        total += byte;
    EXPECT_EQ(total, 4 * 0xab);
}

TEST_CASE(comparison)
{
    constexpr Array a { 1, 2, 3 };
    constexpr Array b { 1, 2, 3 };
    constexpr Array c { 1, 2, 4 };
    static_assert(a == b);
    static_assert(!(a == c));
}
