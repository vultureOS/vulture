/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/poll.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

// Waits until one of the descriptors is ready, or `timeout` milliseconds
// pass (-1: no limit; 0: just look).
int poll(struct pollfd*, nfds_t count, int timeout);

__END_DECLS
