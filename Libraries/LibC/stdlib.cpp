/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The common core of the strto* family: skips space, reads a sign and a
// base prefix, and accumulates digits with overflow detection.
static unsigned long long parse_unsigned(char const* string, char** end, int base, bool& negative, bool& overflow)
{
    char const* cursor = string;
    while (isspace(*cursor))
        ++cursor;
    negative = false;
    if (*cursor == '+' || *cursor == '-')
        negative = *cursor++ == '-';
    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') && isxdigit(cursor[2])) {
        cursor += 2;
        base = 16;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }

    unsigned long long value = 0;
    overflow = false;
    char const* digits_start = cursor;
    for (;; ++cursor) {
        int digit;
        if (isdigit(*cursor))
            digit = *cursor - '0';
        else if (isalpha(*cursor))
            digit = tolower(*cursor) - 'a' + 10;
        else
            break;
        if (digit >= base)
            break;
        if (value > (ULLONG_MAX - static_cast<unsigned>(digit)) / static_cast<unsigned>(base))
            overflow = true;
        value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
    }
    if (end)
        *end = const_cast<char*>(cursor == digits_start ? string : cursor);
    return value;
}

extern "C" {

long long strtoll(char const* string, char** end, int base)
{
    bool negative, overflow;
    auto magnitude = parse_unsigned(string, end, base, negative, overflow);
    if (overflow || magnitude > static_cast<unsigned long long>(LLONG_MAX) + (negative ? 1 : 0)) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    return negative ? static_cast<long long>(0ull - magnitude) : static_cast<long long>(magnitude);
}

unsigned long long strtoull(char const* string, char** end, int base)
{
    bool negative, overflow;
    auto magnitude = parse_unsigned(string, end, base, negative, overflow);
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? 0ull - magnitude : magnitude;
}

long strtol(char const* string, char** end, int base)
{
    return static_cast<long>(strtoll(string, end, base));
}

unsigned long strtoul(char const* string, char** end, int base)
{
    return static_cast<unsigned long>(strtoull(string, end, base));
}

int atoi(char const* string)
{
    return static_cast<int>(strtol(string, nullptr, 10));
}

long atol(char const* string)
{
    return strtol(string, nullptr, 10);
}

long long atoll(char const* string)
{
    return strtoll(string, nullptr, 10);
}

char* getenv(char const* name)
{
    if (!environ)
        return nullptr;
    size_t length = strlen(name);
    for (char** entry = environ; *entry; ++entry) {
        if (strncmp(*entry, name, length) == 0 && (*entry)[length] == '=')
            return *entry + length + 1;
    }
    return nullptr;
}

int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }
long long llabs(long long value) { return value < 0 ? -value : value; }

static void swap_bytes(char* a, char* b, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        char temporary = a[i];
        a[i] = b[i];
        b[i] = temporary;
    }
}

// Insertion sort for short runs, otherwise quicksort around a median of three.
static void sort(char* base, size_t count, size_t size, int (*compare)(void const*, void const*))
{
    while (count > 8) {
        char* first = base;
        char* middle = base + (count / 2) * size;
        char* last = base + (count - 1) * size;
        if (compare(middle, first) < 0)
            swap_bytes(middle, first, size);
        if (compare(last, middle) < 0) {
            swap_bytes(last, middle, size);
            if (compare(middle, first) < 0)
                swap_bytes(middle, first, size);
        }
        swap_bytes(middle, last, size);
        char* pivot = last;
        size_t store = 0;
        for (size_t i = 0; i < count - 1; ++i) {
            if (compare(base + i * size, pivot) < 0) {
                swap_bytes(base + i * size, base + store * size, size);
                ++store;
            }
        }
        swap_bytes(base + store * size, pivot, size);
        // Recurse into the smaller side, loop on the larger.
        size_t left = store;
        size_t right = count - store - 1;
        if (left < right) {
            sort(base, left, size, compare);
            base += (store + 1) * size;
            count = right;
        } else {
            sort(base + (store + 1) * size, right, size, compare);
            count = left;
        }
    }
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = i; j > 0 && compare(base + (j - 1) * size, base + j * size) > 0; --j)
            swap_bytes(base + (j - 1) * size, base + j * size, size);
    }
}

void qsort(void* base, size_t count, size_t size, int (*compare)(void const*, void const*))
{
    sort(static_cast<char*>(base), count, size, compare);
}

static unsigned long s_random_state = 1;

int rand()
{
    // A 64-bit linear congruential generator; the top bits are the best.
    s_random_state = s_random_state * 6364136223846793005ul + 1442695040888963407ul;
    return static_cast<int>(s_random_state >> 33);
}

void srand(unsigned seed)
{
    s_random_state = seed;
}
}
