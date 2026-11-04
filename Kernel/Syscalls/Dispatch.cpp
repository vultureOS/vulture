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

// What's waiting for something in the outside world, and has done nothing yet when a signal comes. It should carry on, if nothing ran that
// the program would want to hear about first: a signal that stopped it, or was ignored, or that it had asked (SA_RESTART) not to be told
// about this way. Everything else, sleeps and waits for what's already there among them, is told EINTR whatever happens, as POSIX has it.
static bool can_be_restarted(u64 number)
{
    switch (static_cast<API::Syscall>(number)) {
    case API::Syscall::read:
    case API::Syscall::write:
    case API::Syscall::readv:
    case API::Syscall::writev:
    case API::Syscall::pread:
    case API::Syscall::pwrite:
    case API::Syscall::open:
    case API::Syscall::openat:
    case API::Syscall::accept:
    case API::Syscall::recvfrom:
    case API::Syscall::sendto:
    case API::Syscall::recvmsg:
    case API::Syscall::sendmsg:
    case API::Syscall::waitpid:
    case API::Syscall::read_nocancel:
    case API::Syscall::write_nocancel:
        return true;
    default:
        return false;
    }
}

// Where pthread_cancel() takes effect, as POSIX lists them, of what goes straight to a system call. (The rest of them, like pthread_join(), are
// the C library's, and use a futex that says so.)
static bool is_cancellation_point(u64 number)
{
    switch (static_cast<API::Syscall>(number)) {
    case API::Syscall::read:
    case API::Syscall::write:
    case API::Syscall::readv:
    case API::Syscall::writev:
    case API::Syscall::pread:
    case API::Syscall::pwrite:
    case API::Syscall::open:
    case API::Syscall::openat:
    case API::Syscall::close:
    case API::Syscall::accept:
    case API::Syscall::connect:
    case API::Syscall::recvfrom:
    case API::Syscall::sendto:
    case API::Syscall::recvmsg:
    case API::Syscall::sendmsg:
    case API::Syscall::nanosleep:
    case API::Syscall::poll:
    case API::Syscall::waitpid:
    case API::Syscall::fsync:
    case API::Syscall::sigsuspend:
    case API::Syscall::sigtimedwait:
        return true;
    default:
        return false;
    }
}

void handle_syscall(TrapFrame& frame)
{
    auto* process = Process::current();
    VERIFY(process);
    SyscallArguments arguments;
    for (size_t i = 0; i < 6; ++i)
        arguments.values[i] = Arch::syscall_argument(frame, i);

    // A thread to be cancelled doesn't get as far as doing this.
    auto& thread = Thread::current();
    bool const cancellation_point = is_cancellation_point(Arch::syscall_number(frame));
    if (cancellation_point) {
        thread.set_in_cancellation_point(true);
        if (thread.cancellation_is_due()) {
            process->enter_cancel_handler(frame);
            thread.set_in_cancellation_point(false);
            return;
        }
    }

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

    if (cancellation_point) {
        // If it had got something done, that's for it to hear about, and the next one's where this happens.
        bool const interrupted = result.is_error() && result.error().is_errno() && result.error().code() == EINTR;
        if (interrupted && thread.cancellation_is_due()) {
            process->enter_cancel_handler(frame);
            thread.set_in_cancellation_point(false);
            return;
        }
        thread.set_in_cancellation_point(false);
    }

    if (result.is_error()) {
        // Errors without an errno are internal; report them as I/O errors.
        int code = result.error().is_errno() ? result.error().code() : EIO;
        if (code == EINTR && can_be_restarted(Arch::syscall_number(frame)))
            Thread::current().note_syscall_interrupted(Arch::syscall_number(frame), arguments.values[0]);
        Arch::set_syscall_result(frame, static_cast<u64>(-static_cast<i64>(code)));
    } else {
        Arch::set_syscall_result(frame, result.value());
    }
}

}
