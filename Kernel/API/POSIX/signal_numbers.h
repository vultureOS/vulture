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
#define SI_KERNEL 0x80
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

// Pushed on the user stack when a handler runs; sigreturn restores from it.
// A callee-saved register (x19, or rbx) holds its address when the handler is
// called (the restorer passes it back), and it is also the handler's third
// argument.
struct vulture_signal_frame {
#if defined(__x86_64__)
    unsigned long long rax, rbx, rcx, rdx, rsi, rdi, rbp;
    unsigned long long r8, r9, r10, r11, r12, r13, r14, r15;
#else
    unsigned long long x[31];
#endif
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
    unsigned long long saved_mask;
    unsigned long long magic;
    struct vulture_siginfo info;
#if defined(__x86_64__)
    // As fxsave64 writes it.
    unsigned long long fp_state[64];
#else
    // q0-q31, FPCR and FPSR: a handler may use them freely.
    unsigned long long fp_state[66];
#endif
};

// sigaltstack(): another stack for handlers installed with SA_ONSTACK, for
// one thread (say, to survive overflowing the normal one).
struct vulture_stack {
    void* ss_sp;
    int ss_flags;
    unsigned long ss_size;
};

#define SS_ONSTACK 0x0001
#define SS_DISABLE 0x0004
#define MINSIGSTKSZ 32768
#define SIGSTKSZ 131072

#define VULTURE_SIGNAL_FRAME_MAGIC 0x5349474e414c4652ull
