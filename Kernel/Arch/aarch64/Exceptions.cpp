/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/aarch64/GICv3.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Tasks/Scheduler.h>

// Vector 5: an IRQ taken at EL1 on SP_EL1, which is how the kernel runs.
static constexpr u64 irq_from_kernel = 5;

// Called from Vectors.S for every exception. Interrupts from the kernel go
// to the interrupt controller; every other exception is fatal for now.
extern "C" void handle_exception(Kernel::TrapFrame* frame, u64 vector_index)
{
    if (vector_index == irq_from_kernel) {
        Kernel::AArch64::handle_irq();
        // A switch from here resumes the interrupted thread later, on its
        // own stack, through the rest of this exception's return path.
        Kernel::Scheduler::preempt_if_needed();
        return;
    }

    FixedStringBuilder<192> reason;
    Kernel::Arch::describe_exception(reason, *frame, vector_index);
    Kernel::panic(reason.view(), frame);
}
