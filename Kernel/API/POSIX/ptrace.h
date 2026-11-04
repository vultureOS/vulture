/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// ptrace's, with the numbers a Mac has for those it has. A Mac's debuggers look in through the kernel's task ports for the rest, so what
// to do about memory and registers is this system's own.
#define PT_TRACE_ME 0
#define PT_CONTINUE 7
#define PT_KILL 8
#define PT_STEP 9
#define PT_ATTACH 10
#define PT_DETACH 11
#define PT_DENY_ATTACH 31
// The address is a struct vulture_ptrace_io.
#define VULTURE_PT_IO 32
// The address is a struct vulture_thread_state: of the thread that stopped.
#define VULTURE_PT_GETREGS 33
#define VULTURE_PT_SETREGS 34

#define VULTURE_PTRACE_READ 1
#define VULTURE_PTRACE_WRITE 2

struct vulture_ptrace_io {
    unsigned long long operation;
    void* target_address; // In the traced program.
    void* local_address;  // In this one.
    unsigned long long length; // In: how much. Out: how much was done.
};

#if defined(__aarch64__)
struct vulture_thread_state {
    unsigned long long x[31];
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};
#else
struct vulture_thread_state {
    unsigned long long r15, r14, r13, r12, r11, r10, r9, r8;
    unsigned long long rdi, rsi, rbp, rdx, rcx, rbx, rax;
    unsigned long long rip;
    unsigned long long rflags;
    unsigned long long rsp;
};
#endif

// What's first in a core dump (Meta: an ELF core file), in a note of this type with the name "VULTURE".
#define VULTURE_NOTE_THREAD 1

struct vulture_core_thread {
    int signal;
    int pid;
    char name[32];
    struct vulture_thread_state state;
};
