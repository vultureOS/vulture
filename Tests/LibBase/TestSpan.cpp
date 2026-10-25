/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Span.h>
#include <LibTest/TestCase.h>

TEST_CASE(basics)
{
    int numbers[] { 1, 2, 3, 4, 5 };
    Span<int> span = numbers;
    EXPECT_EQ(span.size(), 5u);
    EXPECT(!span.is_empty());
    EXPECT_EQ(span.size_in_bytes(), 5 * sizeof(int));
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span.first(), 1);
    EXPECT_EQ(span.last(), 5);

    Span<int> empty;
    EXPECT(empty.is_empty());
    EXPECT(empty.data() == nullptr);
}

TEST_CASE(slicing)
{
    int numbers[] { 10, 20, 30, 40, 50 };
    Span<int> span = numbers;

    auto middle = span.slice(1, 3);
    EXPECT_EQ(middle.size(), 3u);
    EXPECT_EQ(middle[0], 20);
    EXPECT_EQ(middle[2], 40);

    auto tail = span.slice(3);
    EXPECT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0], 40);

    EXPECT(span.slice(5).is_empty());
    EXPECT_EQ(span.trim(2).size(), 2u);
    EXPECT_EQ(span.trim(100).size(), 5u);
}

TEST_CASE(iteration_and_mutation)
{
    int numbers[] { 1, 2, 3 };
    Span<int> span = numbers;

    int sum = 0;
    for (int value : span)
        sum += value;
    EXPECT_EQ(sum, 6);

    span[1] = 20;
    EXPECT_EQ(numbers[1], 20);

    span.fill(7);
    EXPECT_EQ(numbers[0], 7);
    EXPECT_EQ(numbers[2], 7);
}

TEST_CASE(const_conversion_and_copy)
{
    u8 source[] { 1, 2, 3, 4 };
    u8 destination[] { 0, 0 };

    Bytes bytes = source;
    ReadonlyBytes readonly = bytes;
    EXPECT_EQ(readonly.size(), 4u);

    size_t copied = readonly.copy_to(destination);
    EXPECT_EQ(copied, 2u);
    EXPECT_EQ(destination[0], 1);
    EXPECT_EQ(destination[1], 2);
}
