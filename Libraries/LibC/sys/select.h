/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <signal.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/time.h>
#include <sys/types.h>

__BEGIN_DECLS

#define FD_SETSIZE 1024
#define __NFDBITS 32

typedef struct fd_set {
    unsigned int fds_bits[FD_SETSIZE / __NFDBITS];
} fd_set;

#define FD_ZERO(set) memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / __NFDBITS] |= (1u << ((fd) % __NFDBITS)))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / __NFDBITS] &= ~(1u << ((fd) % __NFDBITS)))
#define FD_ISSET(fd, set) (((set)->fds_bits[(fd) / __NFDBITS] & (1u << ((fd) % __NFDBITS))) != 0)
#define FD_COPY(from, to) memcpy((to), (from), sizeof(fd_set))

int select(int count, fd_set* read_set, fd_set* write_set, fd_set* error_set, struct timeval* timeout);
int pselect(int count, fd_set* read_set, fd_set* write_set, fd_set* error_set, struct timespec const* timeout, sigset_t const* mask);

__END_DECLS
