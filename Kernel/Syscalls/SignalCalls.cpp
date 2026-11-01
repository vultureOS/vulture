/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/Time.h>

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
    auto& thread = Thread::current();
    u64 old_mask = thread.m_signal_mask.load();
    if (set_address) {
        u64 set = TRY(copy_from_user<u64>(set_address));
        u64 mask = old_mask;
        switch (how) {
        case SIG_BLOCK:
            mask |= set;
            break;
        case SIG_UNBLOCK:
            mask &= ~set;
            break;
        case SIG_SETMASK:
            mask = set;
            break;
        default:
            return Error::from_errno(EINVAL);
        }
        mask &= ~(bit(SIGKILL) | bit(SIGSTOP));
        thread.m_signal_mask.store(mask);
        // A signal this thread was to take now waits for another.
        reroute_pending_signals(m_pending_signals.load() & mask & ~old_mask);
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
    Thread::current().m_signal_mask.store(saved_mask.value() & ~(bit(SIGKILL) | bit(SIGSTOP)));
    // The dispatcher stores this in x0: give back the restored x0.
    return Arch::syscall_argument(frame, 0);
}

ErrorOr<FlatPtr> Process::sys_sigsuspend(TrapFrame&, SyscallArguments const& arguments)
{
    u64 mask = TRY(copy_from_user<u64>(arguments.values[0]));
    auto& thread = Thread::current();
    thread.m_mask_after_suspend = thread.m_signal_mask.load();
    thread.m_signal_mask.store(mask & ~(bit(SIGKILL) | bit(SIGSTOP)));
    // Until a signal arrives that the new mask lets through; it is handled
    // on the way out, and sigsuspend() returns EINTR.
    (void)m_suspend_wait.wait_until_interruptible([] { return false; });
    if (!has_deliverable_signal() && thread.m_mask_after_suspend.has_value())
        thread.m_signal_mask.store(thread.m_mask_after_suspend.release_value());
    return Error::from_errno(EINTR);
}

ErrorOr<FlatPtr> Process::sys_sigpending(TrapFrame&, SyscallArguments const& arguments)
{
    auto& thread = Thread::current();
    u64 pending = (m_pending_signals.load() | thread.m_pending_signals.load()) & thread.m_signal_mask.load();
    TRY(copy_to_user(arguments.values[0], pending));
    return 0;
}

// sigtimedwait(set, info, timeout): takes a pending signal in `set` (blocked
// by the caller, or it might be delivered first), waiting for one if need be:
// its number, or EAGAIN after the relative `timeout`, or EINTR for another
// signal.
ErrorOr<FlatPtr> Process::sys_sigtimedwait(TrapFrame&, SyscallArguments const& arguments)
{
    u64 set = TRY(copy_from_user<u64>(arguments.values[0])) & ~(bit(SIGKILL) | bit(SIGSTOP));
    FlatPtr info_address = arguments.values[1];
    u64 deadline = 0;
    bool timed = arguments.values[2] != 0;
    if (timed) {
        auto timeout = TRY(copy_from_user<vulture_timespec>(arguments.values[2]));
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1'000'000'000)
            return Error::from_errno(EINVAL);
        deadline = Time::now() + Time::ticks_from_nanoseconds(static_cast<u64>(timeout.tv_sec) * 1'000'000'000 + static_cast<u64>(timeout.tv_nsec));
    }
    auto& thread = Thread::current();
    auto is_there = [&] { return ((m_pending_signals.load() | thread.m_pending_signals.load()) & set) != 0; };
    for (;;) {
        int signal = 0;
        vulture_siginfo info {};
        {
            SpinLockGuard guard(m_signal_lock);
            if (u64 own = thread.m_pending_signals.load() & set) {
                signal = __builtin_ctzll(own);
                thread.m_pending_signals.fetch_and(~bit(signal));
                info = thread.m_signal_info[signal];
            } else if (u64 shared = m_pending_signals.load() & set) {
                signal = __builtin_ctzll(shared);
                m_pending_signals.fetch_and(~bit(signal));
                info = m_signal_info[signal];
            }
        }
        if (signal) {
            if (info_address)
                TRY(copy_to_user(info_address, info));
            return static_cast<FlatPtr>(signal);
        }
        if (timed) {
            auto result = m_suspend_wait.wait_until_interruptible(is_there, deadline);
            if (result == WaitQueue::WaitResult::Interrupted)
                return Error::from_errno(EINTR);
            if (result == WaitQueue::WaitResult::TimedOut)
                return Error::from_errno(EAGAIN);
        } else if (!m_suspend_wait.wait_until_interruptible(is_there)) {
            return Error::from_errno(EINTR);
        }
    }
}

ErrorOr<FlatPtr> Process::sys_sigaltstack(TrapFrame& frame, SyscallArguments const& arguments)
{
    auto& thread = Thread::current();
    FlatPtr sp = Arch::user_stack_pointer(frame);
    bool on_it = thread.m_alternate_stack_size && sp >= thread.m_alternate_stack && sp - thread.m_alternate_stack < thread.m_alternate_stack_size;
    if (arguments.values[1]) {
        vulture_stack old {};
        old.ss_sp = reinterpret_cast<void*>(thread.m_alternate_stack);
        old.ss_size = thread.m_alternate_stack_size;
        old.ss_flags = on_it ? SS_ONSTACK : (thread.m_alternate_stack_size ? 0 : SS_DISABLE);
        TRY(copy_to_user(arguments.values[1], old));
    }
    if (arguments.values[0]) {
        auto stack = TRY(copy_from_user<vulture_stack>(arguments.values[0]));
        if (stack.ss_flags & ~SS_DISABLE)
            return Error::from_errno(EINVAL);
        if (on_it)
            return Error::from_errno(EPERM);
        if (stack.ss_flags & SS_DISABLE) {
            thread.m_alternate_stack = 0;
            thread.m_alternate_stack_size = 0;
        } else {
            if (stack.ss_size < MINSIGSTKSZ)
                return Error::from_errno(ENOMEM);
            thread.m_alternate_stack = reinterpret_cast<FlatPtr>(stack.ss_sp);
            thread.m_alternate_stack_size = stack.ss_size;
        }
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_alarm(TrapFrame&, SyscallArguments const& arguments)
{
    u64 seconds = arguments.values[0];
    MutexLocker locker(m_alarm_lock);
    u64 now = Time::now();
    // What was left of the previous alarm, rounded up.
    u64 remaining = 0;
    if (m_alarm_timer.cancel() && m_alarm_deadline > now)
        remaining = (Time::microseconds_from_ticks(m_alarm_deadline - now) + 999'999) / 1'000'000;
    m_alarm_deadline = 0;
    if (seconds > 0) {
        m_alarm_deadline = now + Time::ticks_from_microseconds(seconds * 1'000'000);
        m_alarm_timer.context = this;
        InterruptDisabler disabler;
        Processor::current().timer_queue().add(m_alarm_timer, m_alarm_deadline, [](Timer& timer) {
            vulture_siginfo info {};
            info.si_code = SI_KERNEL;
            static_cast<Process*>(timer.context)->send_signal(SIGALRM, info);
        });
    }
    return remaining;
}

}
