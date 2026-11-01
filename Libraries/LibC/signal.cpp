/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <signal.h>

using System::Syscall;

static_assert(static_cast<unsigned long>(Syscall::sigreturn) == 16, "the restorer below hard-codes the sigreturn number");

// Where a signal handler returns to. The kernel left the signal frame's
// address in x19 (callee-saved, so the handler kept it); sigreturn finds
// the frame there and resumes the interrupted code.
extern "C" [[gnu::naked]] void __vulture_signal_restorer()
{
    asm volatile("mov x8, #16\n"
                 "svc #0\n"
                 "brk #0\n");
}

static int check(long result)
{
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<int>(result);
}

extern "C" {

int sigaction(int signal, struct sigaction const* action, struct sigaction* old_action)
{
    vulture_sigaction raw_new {};
    vulture_sigaction raw_old {};
    if (action) {
        raw_new.handler = reinterpret_cast<unsigned long long>(action->sa_handler);
        raw_new.mask = action->sa_mask;
        raw_new.flags = static_cast<unsigned int>(action->sa_flags) | SA_RESTORER;
        raw_new.restorer = reinterpret_cast<unsigned long long>(&__vulture_signal_restorer);
    }
    int result = check(System::syscall(Syscall::sigaction, static_cast<unsigned long>(signal),
        action ? reinterpret_cast<unsigned long>(&raw_new) : 0, old_action ? reinterpret_cast<unsigned long>(&raw_old) : 0));
    if (result == 0 && old_action) {
        old_action->sa_handler = reinterpret_cast<sighandler_t>(raw_old.handler);
        old_action->sa_mask = raw_old.mask;
        old_action->sa_flags = static_cast<int>(raw_old.flags & ~SA_RESTORER);
        old_action->sa_restorer = nullptr;
    }
    return result;
}

sighandler_t signal(int number, sighandler_t handler)
{
    struct sigaction action {};
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    struct sigaction old_action {};
    if (sigaction(number, &action, &old_action) < 0)
        return SIG_ERR;
    return old_action.sa_handler;
}

int sigprocmask(int how, sigset_t const* set, sigset_t* old_set)
{
    return check(System::syscall(Syscall::sigprocmask, static_cast<unsigned long>(how), reinterpret_cast<unsigned long>(set), reinterpret_cast<unsigned long>(old_set)));
}

int sigemptyset(sigset_t* set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set)
{
    *set = ~0ul;
    return 0;
}

int sigaddset(sigset_t* set, int signal)
{
    if (signal <= 0 || signal >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1ul << signal;
    return 0;
}

int sigdelset(sigset_t* set, int signal)
{
    if (signal <= 0 || signal >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1ul << signal);
    return 0;
}

int sigismember(sigset_t const* set, int signal)
{
    if (signal <= 0 || signal >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return (*set >> signal) & 1;
}
}

char const* const sys_signame[NSIG] = {
    "zero", "hup", "int", "quit", "ill", "trap", "abrt", "bus", "fpe", "kill", "usr1", "segv", "usr2", "pipe", "alrm", "term",
    "stkflt", "chld", "cont", "stop", "tstp", "ttin", "ttou", "urg", "xcpu", "xfsz", "vtalrm", "prof", "winch", "io", "pwr", "sys",
};
