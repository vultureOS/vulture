/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

__BEGIN_DECLS

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval*, void* zone);

// Timers that send a signal, once or over and over. ITIMER_REAL sends SIGALRM by the clock on the wall; ITIMER_VIRTUAL sends
// SIGVTALRM, and ITIMER_PROF SIGPROF, by how much processor time this process has had.
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

int setitimer(int which, struct itimerval const* value, struct itimerval* previous);
int getitimer(int which, struct itimerval* value);

__END_DECLS
