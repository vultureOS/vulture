/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/StdLibExtras.h>

namespace Base {

namespace Detail {

template<typename T, typename LessThan>
constexpr void insertion_sort(T* data, size_t size, LessThan& less_than)
{
    for (size_t i = 1; i < size; ++i) {
        for (size_t j = i; j > 0 && less_than(data[j], data[j - 1]); --j)
            swap(data[j], data[j - 1]);
    }
}

}

// Sorts in place (not stably) in O(n log n) on average: quicksort with a
// median-of-three pivot, recursing only into the smaller part (so the
// stack stays O(log n)) and finishing small parts by insertion.
template<typename T, typename LessThan>
constexpr void quick_sort(Span<T> items, LessThan less_than)
{
    T* data = items.data();
    size_t size = items.size();
    while (size > 16) {
        size_t middle = size / 2;
        if (less_than(data[middle], data[0]))
            swap(data[middle], data[0]);
        if (less_than(data[size - 1], data[0]))
            swap(data[size - 1], data[0]);
        if (less_than(data[size - 1], data[middle]))
            swap(data[size - 1], data[middle]);
        // The median now sits in the middle; park it at the end.
        swap(data[middle], data[size - 1]);
        T& pivot = data[size - 1];
        size_t store = 0;
        for (size_t i = 0; i + 1 < size; ++i) {
            if (less_than(data[i], pivot))
                swap(data[i], data[store++]);
        }
        swap(data[store], data[size - 1]);
        size_t left = store, right = size - store - 1;
        if (left < right) {
            quick_sort(Span<T>(data, left), less_than);
            data += store + 1;
            size = right;
        } else {
            quick_sort(Span<T>(data + store + 1, right), less_than);
            size = left;
        }
    }
    Detail::insertion_sort(data, size, less_than);
}

template<typename T>
constexpr void quick_sort(Span<T> items)
{
    quick_sort(items, [](T const& a, T const& b) { return a < b; });
}

}

using Base::quick_sort;
