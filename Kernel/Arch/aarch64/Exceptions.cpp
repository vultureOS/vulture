/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/aarch64/GIC.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Syscalls/Dispatch.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Scheduler.h>

using namespace Kernel;

namespace {

// Vector table entries (Vectors.S): four kinds from four sources.
constexpr u64 synchronous_from_kernel = 4;
constexpr u64 irq_from_kernel = 5;
constexpr u64 synchronous_from_user = 8;
constexpr u64 irq_from_user = 9;

// ESR_EL1 exception classes.
constexpr u64 class_unknown = 0x00;
constexpr u64 class_branch_target = 0x0d;
constexpr u64 class_illegal_state = 0x0e;
constexpr u64 class_pointer_authentication = 0x1c;
constexpr u64 class_svc = 0x15;
constexpr u64 class_instruction_abort_user = 0x20;
constexpr u64 class_pc_alignment = 0x22;
constexpr u64 class_data_abort_user = 0x24;
constexpr u64 class_data_abort_kernel = 0x25;
constexpr u64 class_sp_alignment = 0x26;
constexpr u64 class_breakpoint = 0x3c;

constexpr u64 spsr_irq_masked = 1 << 7;

u64 exception_class(TrapFrame const& frame)
{
    return (frame.esr >> 26) & 0x3f;
}

// Data/instruction fault status: translation, access flag or permission
// faults are the page faults; anything else is a real error.
bool is_page_fault(TrapFrame const& frame)
{
    u64 status = frame.esr & 0x3f;
    return (status >> 2) >= 0b0001 && (status >> 2) <= 0b0011;
}

bool is_permission_fault(TrapFrame const& frame)
{
    return ((frame.esr & 0x3f) >> 2) == 0b0011;
}

AccessKind access_kind(TrapFrame const& frame)
{
    if (exception_class(frame) == class_instruction_abort_user)
        return AccessKind::Execute;
    return (frame.esr & (1 << 6)) ? AccessKind::Write : AccessKind::Read;
}

void handle_user_synchronous(TrapFrame& frame)
{
    auto& process = *Process::current();
    // The program could not run on: only it can be waiting for this.
    Arch::enable_interrupts();
    switch (exception_class(frame)) {
    case class_svc:
        handle_syscall(frame);
        break;
    case class_instruction_abort_user:
    case class_data_abort_user: {
        if (!is_page_fault(frame)) {
            process.handle_user_exception(frame, SIGBUS);
            break;
        }
        auto access = access_kind(frame);
        auto result = process.address_space()->handle_fault(frame.far, access);
        if (result != PageFaultResult::Resolved)
            process.handle_user_fault(frame, frame.far, access, is_permission_fault(frame), result);
        break;
    }
    case class_pc_alignment:
    case class_sp_alignment:
        process.handle_user_exception(frame, SIGBUS);
        break;
    case class_branch_target:
    case class_pointer_authentication:
        // A jump to somewhere that isn't a landing pad, or a return address that doesn't check out.
        process.handle_user_exception(frame, SIGILL);
        break;
    case class_breakpoint:
        process.handle_user_exception(frame, SIGTRAP);
        break;
    case class_unknown:
    case class_illegal_state:
    default:
        process.handle_user_exception(frame, SIGILL);
        break;
    }
    Arch::mask_interrupts();
}

// The last stop before user mode: switch threads if asked to, and do what
// the process has for this thread (deliver signals, stop, end).
void prepare_return_to_user(TrapFrame& frame)
{
    for (;;) {
        Scheduler::preempt_if_needed();
        auto* process = Process::current();
        if (!process || !process->has_work_before_user())
            return;
        Arch::enable_interrupts();
        process->do_work_before_user(frame);
        Arch::mask_interrupts();
    }
}

// The kernel faulted touching a user address: on the program's behalf, in
// the user access routines. Map the page if the program may have it there;
// otherwise resume at the routine's recovery address (EFAULT).
bool handle_kernel_fault_on_user_memory(TrapFrame& frame)
{
    if (exception_class(frame) != class_data_abort_kernel || !is_page_fault(frame) || !is_user_range(frame.far, 1))
        return false;
    auto fixup = user_access_fixup(frame.elr);
    auto* process = Process::current();
    if (!fixup.has_value() || !process)
        return false;
    // The interrupted code ran with interrupts enabled (user access never
    // happens under a spin lock), so the fault may sleep too.
    if (!(frame.spsr & spsr_irq_masked))
        Arch::enable_interrupts();
    auto result = process->address_space()->handle_fault(frame.far, access_kind(frame));
    Arch::mask_interrupts();
    if (result != PageFaultResult::Resolved)
        frame.elr = fixup.value();
    return true;
}

}

// Called from Vectors.S for every exception.
extern "C" void handle_exception(TrapFrame* frame, u64 vector_index)
{
    switch (vector_index) {
    case irq_from_kernel:
        AArch64::handle_irq();
        // A switch from here resumes the interrupted thread later, on its
        // own stack, through the rest of this exception's return path.
        Scheduler::preempt_if_needed();
        return;
    case irq_from_user:
        AArch64::handle_irq();
        prepare_return_to_user(*frame);
        return;
    case synchronous_from_user:
        handle_user_synchronous(*frame);
        prepare_return_to_user(*frame);
        return;
    case synchronous_from_kernel:
        if (handle_kernel_fault_on_user_memory(*frame))
            return;
        break;
    default:
        break;
    }

    FixedStringBuilder<192> reason;
    Arch::describe_exception(reason, *frame, vector_index);
    panic(reason.view(), frame);
}
