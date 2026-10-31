/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Debug/Panic.h>

// Called from Vectors.S for every exception. Interrupts are not enabled yet
// (they arrive with the GIC in M2), so every exception is a fatal error.
extern "C" void handle_exception(Kernel::TrapFrame* frame, u64 vector_index)
{
    FixedStringBuilder<192> reason;
    Kernel::Arch::describe_exception(reason, *frame, vector_index);
    Kernel::panic(reason.view(), frame);
}
