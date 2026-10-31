/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// The CPU state saved on exception entry. Vectors.S writes this layout, so
// the offsets below must stay in sync with it.
struct TrapFrame {
    u64 x[31]; // x29 is the frame pointer, x30 the link register.
    u64 sp;
    u64 elr; // Where the exception was taken.
    u64 spsr;
    u64 esr; // Why: the exception class and details.
    u64 far; // The faulting address, for aborts.
    // A frame record {interrupted x29, elr} that the handler's frame
    // pointer chain runs through.
    u64 frame_record[2];
    u64 user_sp; // SP_EL0: the user stack pointer.
    u64 reserved;

    [[nodiscard]] FlatPtr program_counter() const { return elr; }
    [[nodiscard]] FlatPtr frame_pointer() const { return x[29]; }
    // SPSR.M[3:0] is 0 (EL0t) when the exception came from user mode.
    [[nodiscard]] bool is_from_user() const { return (spsr & 0xf) == 0; }
};

static_assert(sizeof(TrapFrame) == 320);
static_assert(__builtin_offsetof(TrapFrame, user_sp) == 304);
static_assert(__builtin_offsetof(TrapFrame, sp) == 248);
static_assert(__builtin_offsetof(TrapFrame, far) == 280);

}
