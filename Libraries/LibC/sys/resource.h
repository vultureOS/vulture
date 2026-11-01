/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// Resource limits. The kernel's own are fixed (64 open files, an 8 MiB
// stack); these report them, and a program may lower its soft limits
// (which last until it execs). Not yet enforced beyond the kernel's.

typedef unsigned long long rlim_t;
#define RLIM_INFINITY (~(rlim_t)0)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_AS 5
#define RLIMIT_RSS RLIMIT_AS
#define RLIMIT_MEMLOCK 6
#define RLIMIT_NPROC 7
#define RLIMIT_NOFILE 8
#define RLIM_NLIMITS 9

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

int getrlimit(int resource, struct rlimit*);
int setrlimit(int resource, struct rlimit const*);

__END_DECLS
