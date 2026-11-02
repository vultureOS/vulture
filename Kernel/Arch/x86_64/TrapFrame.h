/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// The CPU state saved on entry to the kernel, from an exception, an interrupt or a system call.
// Entry.S writes this layout; the processor itself pushes the last five, and all five are what
// iretq takes off again.
struct TrapFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rdx, rcx, rbx, rax;
    u64 cr2; // The faulting address, for page faults.
    // A frame record {interrupted rbp, rip} that the handler's frame pointer chain runs through.
    u64 frame_record[2];
    u64 padding; // Keeps the stack aligned at the call into C++.
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;

    [[nodiscard]] FlatPtr program_counter() const { return rip; }
    [[nodiscard]] FlatPtr frame_pointer() const { return rbp; }
    [[nodiscard]] bool is_from_user() const { return (cs & 3) != 0; }
};

static_assert(sizeof(TrapFrame) == 208);
static_assert(__builtin_offsetof(TrapFrame, rbp) == 80);
static_assert(__builtin_offsetof(TrapFrame, cr2) == 120);
static_assert(__builtin_offsetof(TrapFrame, frame_record) == 128);
static_assert(__builtin_offsetof(TrapFrame, vector) == 152);
static_assert(__builtin_offsetof(TrapFrame, rip) == 168);

}
