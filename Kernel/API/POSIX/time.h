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
