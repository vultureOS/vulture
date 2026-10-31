/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/FastRandom.h>
#include <LibBase/QuickSort.h>
#include <LibBase/Vector.h>
#include <LibTest/TestCase.h>

TEST_CASE(sorts_random_sorted_reversed_and_equal_inputs)
{
    FastRandom random(42);
    size_t const sizes[] = { 0, 1, 2, 17, 100, 5000 };
    for (size_t size : sizes) {
        for (int shape = 0; shape < 4; ++shape) {
            Vector<u64> numbers;
            for (size_t i = 0; i < size; ++i) {
                switch (shape) {
                case 0:
                    numbers.append(random.next_below(1000));
                    break;
                case 1:
                    numbers.append(i);
                    break;
                case 2:
                    numbers.append(size - i);
                    break;
                default:
                    numbers.append(7);
                }
            }
            quick_sort(Span<u64>(numbers.begin(), numbers.size()));
            bool sorted = true;
            for (size_t i = 1; i < numbers.size(); ++i)
                sorted = sorted && numbers[i - 1] <= numbers[i];
            EXPECT(sorted);
        }
    }
}

TEST_CASE(sorts_with_a_comparator)
{
    int values[] = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4 };
    quick_sort(Span<int>(values), [](int a, int b) { return a > b; });
    for (size_t i = 1; i < sizeof(values) / sizeof(values[0]); ++i)
        EXPECT(values[i - 1] >= values[i]);
}
