/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>

namespace Kernel {

static constexpr u64 bit(int signal)
{
    return 1ull << signal;
}

ErrorOr<FlatPtr> Process::sys_kill(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    int signal = static_cast<int>(arguments.values[1]);
    if (signal < 0 || signal >= NSIG)
        return Error::from_errno(EINVAL);
    if (pid <= 0)
        return Error::from_errno(ENOSYS); // Process groups arrive with job control (M6).
    auto target = from_pid(pid);
    if (!target)
        return Error::from_errno(ESRCH);
    // Signal 0 only asks whether the process exists.
    if (signal == 0 || target->is_zombie())
        return 0;
    vulture_siginfo info {};
    info.si_code = SI_USER;
    info.si_pid = m_pid;
    target->send_signal(signal, info);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sigaction(TrapFrame&, SyscallArguments const& arguments)
{
    int signal = static_cast<int>(arguments.values[0]);
    FlatPtr new_action_address = arguments.values[1];
    FlatPtr old_action_address = arguments.values[2];
    if (signal <= 0 || signal >= NSIG)
        return Error::from_errno(EINVAL);
    if (new_action_address && (signal == SIGKILL || signal == SIGSTOP))
        return Error::from_errno(EINVAL);

    vulture_sigaction new_action {};
    if (new_action_address) {
        new_action = TRY(copy_from_user<vulture_sigaction>(new_action_address));
        if (!(new_action.flags & SA_RESTORER) && new_action.handler > VULTURE_SIG_IGN)
            return Error::from_errno(EINVAL);
    }
    vulture_sigaction old_action {};
    {
        SpinLockGuard guard(m_signal_lock);
        auto& action = m_signal_actions[signal];
        old_action = { action.handler, action.mask, action.flags, action.restorer };
        if (new_action_address) {
            action = { new_action.handler, new_action.mask & ~(bit(SIGKILL) | bit(SIGSTOP)), new_action.flags, new_action.restorer };
            // Ignoring a pending signal discards it.
            if (action.handler == VULTURE_SIG_IGN)
                m_pending_signals.fetch_and(~bit(signal));
        }
    }
    if (old_action_address)
        TRY(copy_to_user(old_action_address, old_action));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sigprocmask(TrapFrame&, SyscallArguments const& arguments)
{
    int how = static_cast<int>(arguments.values[0]);
    FlatPtr set_address = arguments.values[1];
    FlatPtr old_set_address = arguments.values[2];
    u64 old_mask = m_signal_mask;
    if (set_address) {
        u64 set = TRY(copy_from_user<u64>(set_address));
        SpinLockGuard guard(m_signal_lock);
        switch (how) {
        case SIG_BLOCK:
            m_signal_mask |= set;
            break;
        case SIG_UNBLOCK:
            m_signal_mask &= ~set;
            break;
        case SIG_SETMASK:
            m_signal_mask = set;
            break;
        default:
            return Error::from_errno(EINVAL);
        }
        m_signal_mask &= ~(bit(SIGKILL) | bit(SIGSTOP));
    }
    if (old_set_address)
        TRY(copy_to_user(old_set_address, old_mask));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sigreturn(TrapFrame& frame, SyscallArguments const&)
{
    auto saved_mask = Arch::pop_signal_frame(frame);
    if (saved_mask.is_error()) {
        vulture_siginfo info {};
        force_signal(SIGSEGV, info);
        return 0;
    }
    {
        SpinLockGuard guard(m_signal_lock);
        m_signal_mask = saved_mask.value() & ~(bit(SIGKILL) | bit(SIGSTOP));
    }
    // The dispatcher stores this in x0: give back the restored x0.
    return Arch::syscall_argument(frame, 0);
}

}
