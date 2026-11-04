/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// setrlimit's limits.

#define VULTURE_RLIM_INFINITY 0x7fffffffffffffffull

#define VULTURE_RLIMIT_CPU 0
#define VULTURE_RLIMIT_FSIZE 1
#define VULTURE_RLIMIT_DATA 2
#define VULTURE_RLIMIT_STACK 3
#define VULTURE_RLIMIT_CORE 4
#define VULTURE_RLIMIT_AS 5
#define VULTURE_RLIMIT_MEMLOCK 6
#define VULTURE_RLIMIT_NPROC 7
#define VULTURE_RLIMIT_NOFILE 8
#define VULTURE_RLIM_NLIMITS 9

struct vulture_rlimit {
    unsigned long long rlim_cur;
    unsigned long long rlim_max;
};
