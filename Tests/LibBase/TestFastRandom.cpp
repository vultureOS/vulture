/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/FastRandom.h>
#include <LibTest/TestCase.h>

// Reference values computed independently from the published algorithms.
TEST_CASE(fast_random_matches_reference_sequence)
{
    FastRandom zero(0);
    EXPECT_EQ(zero.next(), 0x99ec5f36cb75f2b4ull);
    EXPECT_EQ(zero.next(), 0xbf6e1f784956452aull);
    EXPECT_EQ(zero.next(), 0x1a5f849d4933e6e0ull);

    FastRandom answer(42);
    EXPECT_EQ(answer.next(), 0x15780b2e0c2ec716ull);
    EXPECT_EQ(answer.next(), 0x6104d9866d113a7eull);
    EXPECT_EQ(answer.next(), 0xae17533239e499a1ull);
}

TEST_CASE(fast_random_ranges)
{
    FastRandom random(7);
    bool seen[10] {};
    for (int i = 0; i < 1000; ++i) {
        u64 value = random.next_below(10);
        EXPECT(value < 10);
        seen[value] = true;
        u64 between = random.next_between(100, 102);
        EXPECT(between >= 100 && between <= 102);
    }
    for (bool was_seen : seen)
        EXPECT(was_seen);
    EXPECT_EQ(random.next_below(1), 0u);
}
