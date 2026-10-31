/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/signal_numbers.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef unsigned long sigset_t;
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
typedef struct vulture_siginfo siginfo_t;

#define SIG_DFL ((sighandler_t)VULTURE_SIG_DFL)
#define SIG_IGN ((sighandler_t)VULTURE_SIG_IGN)
#define SIG_ERR ((sighandler_t) - 1)

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

int sigaction(int signal, struct sigaction const* action, struct sigaction* old_action);
sighandler_t signal(int signal, sighandler_t handler);
int sigprocmask(int how, sigset_t const* set, sigset_t* old_set);
int kill(pid_t, int signal);
int raise(int signal);

int sigemptyset(sigset_t*);
int sigfillset(sigset_t*);
int sigaddset(sigset_t*, int signal);
int sigdelset(sigset_t*, int signal);
int sigismember(sigset_t const*, int signal);

__END_DECLS
