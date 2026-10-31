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

// A user thread's register state, as the arch keeps it in its trap frame.

namespace Kernel::Arch {

// Starts a program: `entry` with `stack_pointer`, every other register zero.
void prepare_user_entry(TrapFrame&, FlatPtr entry, FlatPtr stack_pointer);

// Enters user mode through `frame`, which must sit at the top of the
// current thread's kernel stack.
[[noreturn]] void return_to_user(TrapFrame&);

[[nodiscard]] u64 syscall_number(TrapFrame const&);
[[nodiscard]] u64 syscall_argument(TrapFrame const&, size_t index);
void set_syscall_result(TrapFrame&, u64);
// For fork: the child sees 0.
void set_fork_child_result(TrapFrame&);

[[nodiscard]] FlatPtr user_stack_pointer(TrapFrame const&);

// Runs a signal handler next: pushes a vulture_signal_frame (the current
// state, `saved_mask` and `info`) on the user stack and points the frame at
// handler(signal, &info, &signal_frame), returning to `restorer`.
ErrorOr<void> push_signal_frame(TrapFrame&, int signal, vulture_siginfo const&, u64 saved_mask, FlatPtr handler, FlatPtr restorer);
// sigreturn: restores the state from the signal frame; returns its saved mask.
ErrorOr<u64> pop_signal_frame(TrapFrame&);

// After writing code into a page through another mapping, makes the
// instruction cache see it.
void make_code_coherent(PhysicalAddress page);

}
