/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>

struct vulture_thread_state;

// A user thread's register state, as the arch keeps it in its trap frame.

namespace Kernel::Arch {

// Starts a program: `entry` with `stack_pointer`, every other register zero.
void prepare_user_entry(TrapFrame&, FlatPtr entry, FlatPtr stack_pointer);

// What a new thread's entry function receives as its argument.
void set_user_entry_argument(TrapFrame&, u64);
// A function, rather than a program's first instruction, is about to be entered: leaves the stack as
// a call would have.
void prepare_user_function_entry(TrapFrame&);

// Enters user mode through `frame`, which must sit at the top of the
// current thread's kernel stack.
[[noreturn]] void return_to_user(TrapFrame&);

[[nodiscard]] u64 syscall_number(TrapFrame const&);
[[nodiscard]] u64 syscall_argument(TrapFrame const&, size_t index);
void set_syscall_result(TrapFrame&, u64);
[[nodiscard]] u64 syscall_result(TrapFrame const&);
// For fork: the child sees 0.
void set_fork_child_result(TrapFrame&);

[[nodiscard]] FlatPtr user_stack_pointer(TrapFrame const&);

// Runs a signal handler next: pushes a vulture_signal_frame (the current
// state, `saved_mask` and `info`) on the user stack, or below `stack_top` if
// that is not 0, and points the frame at handler(signal, &info,
// &signal_frame), returning to `restorer`.
ErrorOr<void> push_signal_frame(TrapFrame&, int signal, vulture_siginfo const&, u64 saved_mask, FlatPtr handler, FlatPtr restorer, FlatPtr stack_top);
// sigreturn: restores the state from the signal frame; returns its saved mask.
ErrorOr<u64> pop_signal_frame(TrapFrame&);

// After writing code into a page through another mapping, makes the
// instruction cache see it.
void make_code_coherent(PhysicalAddress page);

// Back to the system call instruction that got it here, to make the same call again: EINTR wasn't the answer after all.
void rewind_syscall(TrapFrame&, u64 number, u64 first_argument);
// Back in user mode, in this function of the program's, on the stack it's on, with nothing to return to.
void enter_user_function(TrapFrame&, FlatPtr function);
// One instruction, then a trap: on the way back to user mode in this frame.
void set_single_step(TrapFrame&, bool on);
// Is a thread that's stepping about to run, or not? (Some processors want telling, not just the frame.)
void note_single_stepping(bool);
// ptrace's registers.
void read_thread_state(TrapFrame const&, vulture_thread_state&);
void write_thread_state(TrapFrame&, vulture_thread_state const&);

}
