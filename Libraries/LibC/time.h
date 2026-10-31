/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/time.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int clock_gettime(clockid_t, struct timespec*);
int nanosleep(struct timespec const* duration, struct timespec* remaining);
time_t time(time_t* result);

__END_DECLS
