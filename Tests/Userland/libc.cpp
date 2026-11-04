/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf s_jump;

[[gnu::noinline]] static void jump_back(int value)
{
    longjmp(s_jump, value);
}

int main()
{
    char buffer[128];

    // printf formatting.
    snprintf(buffer, sizeof(buffer), "%d|%5d|%-5d|%05d|%+d", 42, 42, 42, 42, 42);
    CHECK(strcmp(buffer, "42|   42|42   |00042|+42") == 0);
    snprintf(buffer, sizeof(buffer), "%x|%X|%#x|%o|%u", 255u, 255u, 255u, 8u, 4000000000u);
    CHECK(strcmp(buffer, "ff|FF|0xff|10|4000000000") == 0);
    snprintf(buffer, sizeof(buffer), "%ld|%lld|%zu|%lu", -9000000000l, -1ll, sizeof(int), ULONG_MAX);
    CHECK(strcmp(buffer, "-9000000000|-1|4|18446744073709551615") == 0);
    snprintf(buffer, sizeof(buffer), "[%s|%.3s|%8s|%-8s|%c]", "text", "abcdef", "right", "left", 'Z');
    CHECK(strcmp(buffer, "[text|abc|   right|left    |Z]") == 0);
    snprintf(buffer, sizeof(buffer), "%.2f|%f|%.0f|%5.1f", 3.14159, -2.5, 2.5, 9.96);
    // (2.5 is exactly between 2 and 3, and goes to the even one, as a Mac prints it.)
    CHECK(strcmp(buffer, "3.14|-2.500000|2| 10.0") == 0);
    snprintf(buffer, sizeof(buffer), "%p", reinterpret_cast<void*>(0x1234));
    CHECK(strcmp(buffer, "0x1234") == 0);
    // Truncation still reports the full length (the size is opaque to the
    // compiler here, so it does not warn about the deliberate truncation).
    size_t volatile small = 4;
    CHECK(snprintf(buffer, small, "%s", "abcdef") == 6 && strcmp(buffer, "abc") == 0);
    snprintf(buffer, sizeof(buffer), "%d%%", 50);
    CHECK(strcmp(buffer, "50%") == 0);

    // Strings.
    CHECK(strlen("hello") == 5);
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("same", "same") == 0);
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK(strchr("hello", 'l') && *strchr("hello", 'l') == 'l' && strrchr("hello", 'l') - "hello" == 3);
    CHECK(strstr("needle in haystack", "hay") != nullptr && strstr("abc", "xyz") == nullptr);
    char copy[16];
    strcpy(copy, "first");
    strcat(copy, "+second");
    CHECK(strcmp(copy, "first+second") == 0);
    CHECK(strlcpy(copy, "truncated text", 6) == 14 && strcmp(copy, "trunc") == 0);
    char tokens[] = "a, b,,c";
    char* state;
    CHECK(strcmp(strtok_r(tokens, ", ", &state), "a") == 0);
    CHECK(strcmp(strtok_r(nullptr, ", ", &state), "b") == 0);
    CHECK(strcmp(strtok_r(nullptr, ", ", &state), "c") == 0);
    CHECK(strtok_r(nullptr, ", ", &state) == nullptr);
    char* duplicate = strdup("dup");
    CHECK(duplicate && strcmp(duplicate, "dup") == 0);
    free(duplicate);
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);

    // Numbers.
    CHECK(atoi("  -123abc") == -123);
    char* end;
    CHECK(strtol("0x1F", &end, 0) == 31 && *end == '\0');
    CHECK(strtol("0755", nullptr, 0) == 493);
    CHECK(strtoul("zz", &end, 36) == 1295);
    errno = 0;
    CHECK(strtoll("99999999999999999999", nullptr, 10) == LLONG_MAX && errno == ERANGE);
    CHECK(isdigit('7') && !isdigit('x') && isspace('\t') && toupper('q') == 'Q' && isxdigit('F'));

    // Sorting.
    int values[] = { 5, 3, 9, 1, 7, 2, 8, 6, 4, 0, 15, 11, 13, 12, 14, 10 };
    qsort(values, 16, sizeof(int), [](void const* a, void const* b) { return *static_cast<int const*>(a) - *static_cast<int const*>(b); });
    bool sorted = true;
    for (int i = 0; i < 16; ++i)
        sorted = sorted && values[i] == i;
    CHECK(sorted);

    // setjmp/longjmp.
    int volatile steps = 0;
    int value = setjmp(s_jump);
    steps = steps + 1;
    if (value == 0)
        jump_back(7);
    CHECK(value == 7 && steps == 2);
    return finish();
}
