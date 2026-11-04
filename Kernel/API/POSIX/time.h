/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Clocks for clock_gettime(), shared by the kernel and LibC.

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

struct vulture_timespec {
    long long tv_sec;
    long tv_nsec;
};

#define VULTURE_ITIMER_REAL 0
#define VULTURE_ITIMER_VIRTUAL 1
#define VULTURE_ITIMER_PROF 2

// As <sys/time.h> has them: two of its timevals, each a time_t and microseconds.
struct vulture_itimerval {
    long long interval_seconds;
    long long interval_microseconds;
    long long value_seconds;
    long long value_microseconds;
};
