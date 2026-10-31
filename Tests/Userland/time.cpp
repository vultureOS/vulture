/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <time.h>
#include <unistd.h>

static long long nanoseconds(timespec const& t)
{
    return t.tv_sec * 1'000'000'000ll + t.tv_nsec;
}

int main()
{
    timespec before {}, after {};
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    timespec duration { 0, 20'000'000 };
    CHECK(nanosleep(&duration, nullptr) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    long long elapsed = nanoseconds(after) - nanoseconds(before);
    CHECK(elapsed >= 20'000'000);
    CHECK(elapsed < 1'000'000'000);

    CHECK(usleep(1000) == 0);
    timespec bad { 0, 2'000'000'000 };
    CHECK(nanosleep(&bad, nullptr) == -1 && errno == EINVAL);
    CHECK(clock_gettime(99, &before) == -1 && errno == EINVAL);
    return finish();
}
