/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/ptrace.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/UserAccess.h>

extern "C" [[noreturn]] void aarch64_return_to_user(Kernel::TrapFrame*);
extern "C" void aarch64_save_fp_state(u64*);
extern "C" void aarch64_load_fp_state(u64 const*);

namespace Kernel::Arch {

// SPSR for user mode: EL0t, interrupts unmasked. Only the condition flags
// (N, Z, C, V) are the program's to choose.
static constexpr u64 user_spsr = 0;
static constexpr u64 user_condition_flags = 0xfull << 28;

void prepare_user_entry(TrapFrame& frame, FlatPtr entry, FlatPtr stack_pointer)
{
    __builtin_memset(&frame, 0, sizeof(frame));
    frame.elr = entry;
    frame.user_sp = stack_pointer;
    frame.spsr = user_spsr;
}

void set_user_entry_argument(TrapFrame& frame, u64 value)
{
    frame.x[0] = value;
}

void return_to_user(TrapFrame& frame)
{
    aarch64_return_to_user(&frame);
}

void prepare_user_function_entry(TrapFrame&)
{
    // A call leaves nothing on the stack here.
}

u64 syscall_number(TrapFrame const& frame)
{
    return frame.x[8];
}

u64 syscall_argument(TrapFrame const& frame, size_t index)
{
    VERIFY(index < 6);
    return frame.x[index];
}

void set_syscall_result(TrapFrame& frame, u64 value)
{
    frame.x[0] = value;
}

u64 syscall_result(TrapFrame const& frame)
{
    return frame.x[0];
}

void set_fork_child_result(TrapFrame& frame)
{
    frame.x[0] = 0;
}

FlatPtr user_stack_pointer(TrapFrame const& frame)
{
    return frame.user_sp;
}

ErrorOr<void> push_signal_frame(TrapFrame& frame, int signal, vulture_siginfo const& info, u64 saved_mask, FlatPtr handler, FlatPtr restorer, FlatPtr stack_top)
{
    vulture_signal_frame signal_frame {};
    for (size_t i = 0; i < 31; ++i)
        signal_frame.x[i] = frame.x[i];
    signal_frame.sp = frame.user_sp;
    signal_frame.pc = frame.elr;
    signal_frame.pstate = frame.spsr;
    signal_frame.saved_mask = saved_mask;
    signal_frame.magic = VULTURE_SIGNAL_FRAME_MAGIC;
    signal_frame.info = info;
    aarch64_save_fp_state(reinterpret_cast<u64*>(signal_frame.fp_state));

    // AArch64 has no red zone below the stack pointer; 16-byte aligned.
    FlatPtr address = align_down<FlatPtr>((stack_top ? stack_top : frame.user_sp) - sizeof(signal_frame), 16);
    TRY(copy_to_user(address, signal_frame));

    frame.x[0] = static_cast<u64>(signal);
    frame.x[1] = address + __builtin_offsetof(vulture_signal_frame, info);
    frame.x[2] = address;
    // Callee-saved, so it survives the handler for the restorer to pass on.
    frame.x[19] = address;
    frame.x[30] = restorer;
    frame.user_sp = address;
    frame.elr = handler;
    return {};
}

ErrorOr<u64> pop_signal_frame(TrapFrame& frame)
{
    auto signal_frame = TRY(copy_from_user<vulture_signal_frame>(frame.x[19]));
    if (signal_frame.magic != VULTURE_SIGNAL_FRAME_MAGIC || !is_user_range(signal_frame.pc, 4))
        return Error::from_errno(EFAULT);
    for (size_t i = 0; i < 31; ++i)
        frame.x[i] = signal_frame.x[i];
    frame.user_sp = signal_frame.sp;
    frame.elr = signal_frame.pc;
    // Never let a program raise its privilege through a forged frame.
    frame.spsr = user_spsr | (signal_frame.pstate & user_condition_flags);
    aarch64_load_fp_state(reinterpret_cast<u64 const*>(signal_frame.fp_state));
    return signal_frame.saved_mask;
}

void make_code_coherent(PhysicalAddress page)
{
    u64 line = 4ull << ((READ_SYSTEM_REGISTER(ctr_el0) >> 16) & 0xf);
    auto start = reinterpret_cast<FlatPtr>(physical_to_virtual(page));
    for (FlatPtr address = start; address < start + page_size; address += line)
        asm volatile("dc cvau, %0" ::"r"(address) : "memory");
    // The page will be executed at another address; invalidate everything.
    asm volatile("dsb ish\n"
                 "ic ialluis\n"
                 "dsb ish\n"
                 "isb" ::
                     : "memory");
}

void rewind_syscall(TrapFrame& frame, u64, u64 first_argument)
{
    // svc is 4 bytes; the number's still in x8, as only x0 is ever changed by a call.
    frame.elr -= 4;
    frame.x[0] = first_argument;
}

void enter_user_function(TrapFrame& frame, FlatPtr function)
{
    frame.user_sp = (frame.user_sp - 128) & ~u64(15);
    frame.elr = function;
    frame.x[30] = 0;
    frame.x[29] = 0;
}

void set_single_step(TrapFrame& frame, bool on)
{
    constexpr u64 software_step = 1ull << 21;
    if (on)
        frame.spsr |= software_step;
    else
        frame.spsr &= ~software_step;
}

// With MDSCR_EL1.SS set, a thread that isn't stepping traps before its first instruction; so it's set only while one that is, is running.
void note_single_stepping(bool on)
{
    u64 value;
    asm volatile("mrs %0, mdscr_el1" : "=r"(value));
    u64 const wanted = on ? (value | 1) : (value & ~u64(1));
    if (wanted == value)
        return;
    // The OS lock, which firmware may have left on, would hold it off.
    asm volatile("msr oslar_el1, xzr\n"
                 "msr mdscr_el1, %0\n"
                 "isb" : : "r"(wanted));
}

void read_thread_state(TrapFrame const& frame, vulture_thread_state& state)
{
    for (size_t i = 0; i < 31; ++i)
        state.x[i] = frame.x[i];
    state.sp = frame.user_sp;
    state.pc = frame.elr;
    state.pstate = frame.spsr;
}

void write_thread_state(TrapFrame& frame, vulture_thread_state const& state)
{
    for (size_t i = 0; i < 31; ++i)
        frame.x[i] = state.x[i];
    frame.user_sp = state.sp;
    frame.elr = state.pc;
    // Its flags, and nothing that would have it come back as anything else.
    frame.spsr = (frame.spsr & ~user_condition_flags) | (state.pstate & user_condition_flags);
}

}
