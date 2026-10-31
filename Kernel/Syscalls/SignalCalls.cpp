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

// POSIX: root may signal anyone; others only processes whose real or saved
// user ID matches their real or effective one. SIGCONT may also go to any
// process in the same session (job control).
static bool may_signal(Process const& sender, Process const& target, int signal)
{
    auto const& from = sender.credentials();
    auto const& to = target.credentials();
    if (from.is_superuser())
        return true;
    if (from.uid == to.uid || from.uid == to.saved_uid || from.euid == to.uid || from.euid == to.saved_uid)
        return true;
    return signal == SIGCONT && sender.sid() == target.sid();
}

ErrorOr<FlatPtr> Process::sys_kill(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    int signal = static_cast<int>(arguments.values[1]);
    if (signal < 0 || signal >= NSIG)
        return Error::from_errno(EINVAL);
    vulture_siginfo info {};
    info.si_code = SI_USER;
    info.si_pid = m_pid;
    if (pid == 0 || pid < -1) {
        // 0: our own group; -pgid: that group.
        ProcessID group = pid == 0 ? pgid() : -pid;
        bool any = false, permitted = false;
        for (auto& process : all()) {
            if (process->pgid() != group || process->is_zombie())
                continue;
            any = true;
            if (!may_signal(*this, *process, signal))
                continue;
            permitted = true;
            if (signal != 0)
                process->send_signal(signal, info);
        }
        if (!any)
            return Error::from_errno(ESRCH);
        if (!permitted)
            return Error::from_errno(EPERM);
        return 0;
    }
    if (pid == -1) {
        // Everyone but init (and kernel threads, which have no process).
        bool any = false;
        for (auto& process : all()) {
            if (process->pid() == 1 || process->is_zombie() || !may_signal(*this, *process, signal))
                continue;
            any = true;
            if (signal != 0)
                process->send_signal(signal, info);
        }
        if (!any)
            return Error::from_errno(ESRCH);
        return 0;
    }
    auto target = from_pid(pid);
    if (!target)
        return Error::from_errno(ESRCH);
    // Signal 0 only asks whether the process exists.
    if (!may_signal(*this, *target, signal))
        return Error::from_errno(EPERM);
    if (signal == 0 || target->is_zombie())
        return 0;
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
