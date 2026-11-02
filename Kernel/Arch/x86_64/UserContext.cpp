/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/UserAccess.h>

extern "C" [[noreturn]] void x86_64_return_to_user(Kernel::TrapFrame*);
extern "C" void x86_64_save_fp_state(u8*);
extern "C" void x86_64_load_fp_state(u8 const*);

namespace Kernel::Arch {

// RFLAGS for user mode: interrupts on, and bit 1, which is always set. The arithmetic flags, and the
// direction flag, are the program's to choose: nothing else is.
static constexpr u64 user_rflags = (1 << 9) | (1 << 1);
static constexpr u64 user_settable_flags = 0x08d5 | (1 << 10);

void prepare_user_entry(TrapFrame& frame, FlatPtr entry, FlatPtr stack_pointer)
{
    __builtin_memset(&frame, 0, sizeof(frame));
    frame.rip = entry;
    frame.rsp = stack_pointer;
    frame.rflags = user_rflags;
    frame.cs = X86_64::selector_user_code;
    frame.ss = X86_64::selector_user_data;
}

void set_user_entry_argument(TrapFrame& frame, u64 value)
{
    frame.rdi = value;
}

void prepare_user_function_entry(TrapFrame& frame)
{
    // Where the return address would be. (It doesn't return.)
    frame.rsp -= sizeof(u64);
}

void return_to_user(TrapFrame& frame)
{
    x86_64_return_to_user(&frame);
}

u64 syscall_number(TrapFrame const& frame)
{
    return frame.rax;
}

u64 syscall_argument(TrapFrame const& frame, size_t index)
{
    // As the System V ABI has it for system calls: rcx is taken by the syscall instruction.
    switch (index) {
    case 0:
        return frame.rdi;
    case 1:
        return frame.rsi;
    case 2:
        return frame.rdx;
    case 3:
        return frame.r10;
    case 4:
        return frame.r8;
    case 5:
        return frame.r9;
    default:
        VERIFY_NOT_REACHED();
    }
}

void set_syscall_result(TrapFrame& frame, u64 value)
{
    frame.rax = value;
}

u64 syscall_result(TrapFrame const& frame)
{
    return frame.rax;
}

void set_fork_child_result(TrapFrame& frame)
{
    frame.rax = 0;
}

FlatPtr user_stack_pointer(TrapFrame const& frame)
{
    return frame.rsp;
}

ErrorOr<void> push_signal_frame(TrapFrame& frame, int signal, vulture_siginfo const& info, u64 saved_mask, FlatPtr handler, FlatPtr restorer, FlatPtr stack_top)
{
    vulture_signal_frame signal_frame {};
    signal_frame.rax = frame.rax;
    signal_frame.rbx = frame.rbx;
    signal_frame.rcx = frame.rcx;
    signal_frame.rdx = frame.rdx;
    signal_frame.rsi = frame.rsi;
    signal_frame.rdi = frame.rdi;
    signal_frame.rbp = frame.rbp;
    signal_frame.r8 = frame.r8;
    signal_frame.r9 = frame.r9;
    signal_frame.r10 = frame.r10;
    signal_frame.r11 = frame.r11;
    signal_frame.r12 = frame.r12;
    signal_frame.r13 = frame.r13;
    signal_frame.r14 = frame.r14;
    signal_frame.r15 = frame.r15;
    signal_frame.sp = frame.rsp;
    signal_frame.pc = frame.rip;
    signal_frame.pstate = frame.rflags;
    signal_frame.saved_mask = saved_mask;
    signal_frame.magic = VULTURE_SIGNAL_FRAME_MAGIC;
    signal_frame.info = info;
    alignas(16) u8 fp_state[512];
    x86_64_save_fp_state(fp_state);
    __builtin_memcpy(signal_frame.fp_state, fp_state, sizeof(fp_state));

    // Past the red zone, which the code that was interrupted may be using; a call's alignment, with
    // the return address that is about to be pushed under it.
    FlatPtr const below = (stack_top ? stack_top : frame.rsp - 128) - sizeof(signal_frame);
    FlatPtr const address = align_down<FlatPtr>(below, 16);
    TRY(copy_to_user(address, signal_frame));
    FlatPtr const stack = address - sizeof(u64);
    TRY(copy_to_user(stack, static_cast<u64>(restorer)));

    frame.rdi = static_cast<u64>(signal);
    frame.rsi = address + __builtin_offsetof(vulture_signal_frame, info);
    frame.rdx = address;
    // Callee-saved, so it survives the handler for the restorer to pass on.
    frame.rbx = address;
    frame.rsp = stack;
    frame.rip = handler;
    return {};
}

ErrorOr<u64> pop_signal_frame(TrapFrame& frame)
{
    auto signal_frame = TRY(copy_from_user<vulture_signal_frame>(frame.rbx));
    if (signal_frame.magic != VULTURE_SIGNAL_FRAME_MAGIC || !is_user_range(signal_frame.pc, 4))
        return Error::from_errno(EFAULT);
    frame.rax = signal_frame.rax;
    frame.rbx = signal_frame.rbx;
    frame.rcx = signal_frame.rcx;
    frame.rdx = signal_frame.rdx;
    frame.rsi = signal_frame.rsi;
    frame.rdi = signal_frame.rdi;
    frame.rbp = signal_frame.rbp;
    frame.r8 = signal_frame.r8;
    frame.r9 = signal_frame.r9;
    frame.r10 = signal_frame.r10;
    frame.r11 = signal_frame.r11;
    frame.r12 = signal_frame.r12;
    frame.r13 = signal_frame.r13;
    frame.r14 = signal_frame.r14;
    frame.r15 = signal_frame.r15;
    frame.rsp = signal_frame.sp;
    frame.rip = signal_frame.pc;
    // Never let a program raise its privilege through a forged frame: no segments from there, and
    // only the flags that are its own.
    frame.cs = X86_64::selector_user_code;
    frame.ss = X86_64::selector_user_data;
    frame.rflags = user_rflags | (signal_frame.pstate & user_settable_flags);
    alignas(16) u8 fp_state[512];
    __builtin_memcpy(fp_state, signal_frame.fp_state, sizeof(fp_state));
    // The reserved bits of the SIMD control word fault if set; so does loading them from a forged frame.
    constexpr u32 simd_control_valid = 0xffff;
    u32 simd_control;
    __builtin_memcpy(&simd_control, fp_state + 24, sizeof(simd_control));
    simd_control &= simd_control_valid;
    __builtin_memcpy(fp_state + 24, &simd_control, sizeof(simd_control));
    x86_64_load_fp_state(fp_state);
    return signal_frame.saved_mask;
}

void make_code_coherent(PhysicalAddress)
{
    // The instruction cache keeps up with memory, on this architecture.
}

}
