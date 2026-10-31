/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/Syscall.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Syscalls/Dispatch.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

void handle_syscall(TrapFrame& frame)
{
    auto* process = Process::current();
    VERIFY(process);
    SyscallArguments arguments;
    for (size_t i = 0; i < 6; ++i)
        arguments.values[i] = Arch::syscall_argument(frame, i);

    ErrorOr<FlatPtr> result = Error::from_errno(ENOSYS);
    switch (Arch::syscall_number(frame)) {
#define SYSCALL(name, count)                            \
    case static_cast<u64>(API::Syscall::name):          \
        result = process->sys_##name(frame, arguments); \
        break;
#include <Kernel/API/Syscalls.def>
#undef SYSCALL
    default:
        break;
    }

    if (result.is_error()) {
        // Errors without an errno are internal; report them as I/O errors.
        int code = result.error().is_errno() ? result.error().code() : EIO;
        Arch::set_syscall_result(frame, static_cast<u64>(-static_cast<i64>(code)));
    } else {
        Arch::set_syscall_result(frame, result.value());
    }
}

}
