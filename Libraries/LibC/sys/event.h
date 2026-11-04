/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/event.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

__BEGIN_DECLS

// As vulture_kevent: the kernel's own name for it.
struct kevent {
    unsigned long ident;
    short filter;
    unsigned short flags;
    unsigned int fflags;
    long data;
    void* udata;
};

#define EV_SET(event, a, b, c, d, e, f) \
    do {                                \
        (event)->ident = (a);           \
        (event)->filter = (b);          \
        (event)->flags = (c);           \
        (event)->fflags = (d);          \
        (event)->data = (e);            \
        (event)->udata = (f);           \
    } while (0)

int kqueue(void);
int kevent(int queue, struct kevent const* changes, int change_count, struct kevent* events, int event_count, struct timespec const* timeout);

__END_DECLS
