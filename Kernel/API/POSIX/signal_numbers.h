/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Signal numbers and sigaction flags, shared by the kernel and LibC
// (Linux numbering).

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28
#define NSIG 32

#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO 0x00000004
#define SA_RESTORER 0x04000000
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// Handler values with special meaning.
#define VULTURE_SIG_DFL 0
#define VULTURE_SIG_IGN 1

// The sigaction() system call's argument.
struct vulture_sigaction {
    unsigned long long handler;
    unsigned long long mask;
    unsigned long long flags;
    unsigned long long restorer;
};

// What a handler installed with SA_SIGINFO learns about the signal.
struct vulture_siginfo {
    int si_signo;
    int si_code;
    int si_pid;
    int si_status;
    void* si_addr;
};

#define SI_USER 0
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define CLD_EXITED 1
#define CLD_KILLED 2

// Pushed on the user stack when a handler runs; sigreturn restores from it.
// x19 holds its address when the handler is called (the restorer passes it
// back), and it is also the handler's third argument.
struct vulture_signal_frame {
    unsigned long long x[31];
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
    unsigned long long saved_mask;
    unsigned long long magic;
    struct vulture_siginfo info;
};

#define VULTURE_SIGNAL_FRAME_MAGIC 0x5349474e414c4652ull
