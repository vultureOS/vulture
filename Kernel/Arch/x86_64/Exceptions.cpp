/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/UserContext.h>
#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Syscalls/Dispatch.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>

using namespace Kernel;

namespace {

constexpr u64 vector_divide = 0;
constexpr u64 vector_debug = 1;
constexpr u64 vector_breakpoint = 3;
constexpr u64 vector_overflow = 4;
constexpr u64 vector_invalid_opcode = 6;
constexpr u64 vector_stack_fault = 12;
constexpr u64 vector_general_protection = 13;
constexpr u64 vector_page_fault = 14;
constexpr u64 vector_fp_error = 16;
constexpr u64 vector_alignment = 17;
constexpr u64 vector_simd_error = 19;

constexpr u64 error_present = 1 << 0;
constexpr u64 error_write = 1 << 1;
constexpr u64 error_fetch = 1 << 4;

AccessKind access_kind(TrapFrame const& frame)
{
    if (frame.error_code & error_fetch)
        return AccessKind::Execute;
    return (frame.error_code & error_write) ? AccessKind::Write : AccessKind::Read;
}

void handle_user_exception(TrapFrame& frame)
{
    auto& process = *Process::current();
    // The program could not run on: only it can be waiting for this.
    Arch::enable_interrupts();
    switch (frame.vector) {
    case vector_page_fault: {
        auto const access = access_kind(frame);
        auto const result = process.address_space()->handle_fault(frame.cr2, access);
        if (result != PageFaultResult::Resolved)
            process.handle_user_fault(frame, frame.cr2, access, frame.error_code & error_present, result);
        break;
    }
    case vector_divide:
    case vector_fp_error:
    case vector_simd_error:
        process.handle_user_exception(frame, SIGFPE);
        break;
    case vector_debug:
        // Done: that was the one.
        Thread::current().set_single_stepping(false);
        Arch::set_single_step(frame, false);
        process.handle_user_exception(frame, SIGTRAP);
        break;
    case vector_breakpoint:
    case vector_overflow:
        process.handle_user_exception(frame, SIGTRAP);
        break;
    case vector_invalid_opcode:
        process.handle_user_exception(frame, SIGILL);
        break;
    case vector_alignment:
        process.handle_user_exception(frame, SIGBUS);
        break;
    case vector_stack_fault:
    case vector_general_protection:
    default:
        // Not an address the program has, in a way this can tell it: a non-canonical one, say.
        process.handle_user_exception(frame, SIGSEGV);
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
        if (!process)
            return;
        if (!process->has_work_before_user()) {
            process->finish_interrupted_syscall(frame);
            return;
        }
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
    if (frame.vector != vector_page_fault || !is_user_range(frame.cr2, 1))
        return false;
    auto fixup = user_access_fixup(frame.rip);
    auto* process = Process::current();
    if (!fixup.has_value() || !process)
        return false;
    // The interrupted code ran with interrupts enabled (user access never
    // happens under a spin lock), so the fault may sleep too.
    if (frame.rflags & Arch::rflags_interrupt_enable)
        Arch::enable_interrupts();
    auto const result = process->address_space()->handle_fault(frame.cr2, access_kind(frame));
    Arch::mask_interrupts();
    if (result != PageFaultResult::Resolved)
        frame.rip = fixup.value();
    return true;
}

void handle_interrupt(TrapFrame const& frame)
{
    auto& processor = Processor::current();
    processor.enter_interrupt();
    if (frame.vector >= X86_64::vector_ipi_base && frame.vector < X86_64::vector_ipi_base + ipi_count)
        Processor::handle_ipi(static_cast<IPI>(frame.vector - X86_64::vector_ipi_base));
    else
        dispatch_interrupt(static_cast<u32>(frame.vector));
    X86_64::end_of_interrupt();
    processor.leave_interrupt();
}

}

// Called from Entry.S for everything.
extern "C" void x86_64_handle_exception(TrapFrame* frame)
{
    if (frame->vector == X86_64::vector_syscall) {
        Arch::enable_interrupts();
        handle_syscall(*frame);
        Arch::mask_interrupts();
        prepare_return_to_user(*frame);
        return;
    }
    if (frame->vector == X86_64::vector_spurious)
        return;
    if (frame->vector >= X86_64::first_device_vector) {
        handle_interrupt(*frame);
        if (frame->is_from_user())
            prepare_return_to_user(*frame);
        else
            Scheduler::preempt_if_needed();
        return;
    }
    if (frame->is_from_user()) {
        handle_user_exception(*frame);
        prepare_return_to_user(*frame);
        return;
    }
    if (handle_kernel_fault_on_user_memory(*frame))
        return;

    FixedStringBuilder<192> reason;
    Arch::describe_exception(reason, *frame, frame->vector);
    panic(reason.view(), frame);
}
