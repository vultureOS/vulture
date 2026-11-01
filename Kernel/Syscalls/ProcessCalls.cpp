/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/wait.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/TTY/TTY.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/New.h>

namespace Kernel {

// Defined in Process.cpp.
extern SpinLock& process_tree_lock();

ErrorOr<FlatPtr> Process::sys_exit(TrapFrame&, SyscallArguments const& arguments)
{
    terminate(VULTURE_W_EXITED(static_cast<int>(arguments.values[0])));
}

ErrorOr<FlatPtr> Process::sys_getpid(TrapFrame&, SyscallArguments const&)
{
    return static_cast<FlatPtr>(m_pid);
}

ErrorOr<FlatPtr> Process::sys_getppid(TrapFrame&, SyscallArguments const&)
{
    return static_cast<FlatPtr>(parent_pid());
}

ErrorOr<FlatPtr> Process::sys_gettid(TrapFrame&, SyscallArguments const&)
{
    return static_cast<FlatPtr>(Thread::current().id());
}

ErrorOr<FlatPtr> Process::sys_power_off(TrapFrame&, SyscallArguments const&)
{
    if (m_pid != 1)
        return Error::from_errno(EPERM);
    dmesgln("Powering off at the request of process 1 ({}).", name());
    if (auto result = VirtualFileSystem::sync_all(); result.is_error())
        dmesgln("Power off: could not write everything to disk: {}", result.error());
    Arch::power_off();
}

ErrorOr<FlatPtr> Process::sys_fork(TrapFrame& frame, SyscallArguments const&)
{
    auto child = TRY(create(this));
    child->set_name(name());
    child->m_executable = m_executable;
    child->m_cwd = m_cwd;
    child->m_pgid.store(pgid());
    child->m_sid.store(sid());
    child->m_controlling_terminal = m_controlling_terminal;
    child->m_credentials = m_credentials;
    child->m_umask = m_umask;
    child->m_address_space = TRY(m_address_space->clone_for_fork());
    {
        SpinLockGuard guard(m_files_lock);
        for (size_t fd = 0; fd < max_open_files; ++fd)
            child->m_files[fd] = m_files[fd];
    }
    {
        SpinLockGuard guard(m_signal_lock);
        child->m_signal_mask = m_signal_mask;
        for (int signal = 0; signal < NSIG; ++signal)
            child->m_signal_actions[signal] = m_signal_actions[signal];
    }

    auto thread = TRY(Thread::create(name(), [](void*) {
        Arch::return_to_user(Thread::current().user_trap_frame());
    },
        nullptr, { .qos = Thread::current().qos(), .pinned_processor = {} }));
    // The child resumes from the same point, seeing 0 from fork().
    thread->user_trap_frame() = frame;
    Arch::set_fork_child_result(thread->user_trap_frame());
    thread->set_process(*child);
    child->m_thread = thread.ptr();

    child->register_with_parent();
    thread->start();
    return static_cast<FlatPtr>(child->pid());
}

namespace {

// argv/envp copied into one kernel buffer.
struct ExecArguments {
    static constexpr size_t storage_size = 128 * 1024;
    static constexpr size_t max_strings = 1024;
    char* storage { nullptr };
    size_t used { 0 };
    StringView strings[max_strings];
    size_t count { 0 };

    ~ExecArguments() { kfree(storage); }

    ErrorOr<void> copy_list(FlatPtr list, size_t& first, size_t& length)
    {
        first = count;
        length = 0;
        if (!list)
            return {};
        for (;;) {
            FlatPtr string = TRY(copy_from_user<FlatPtr>(list + length * sizeof(FlatPtr)));
            if (!string)
                return {};
            if (count == max_strings || used >= storage_size)
                return Error::from_errno(E2BIG);
            auto string_length = TRY(copy_string_from_user(string, storage + used, storage_size - used));
            strings[count++] = { storage + used, string_length };
            used += string_length + 1;
            ++length;
        }
    }
};

}

ErrorOr<FlatPtr> Process::sys_execve(TrapFrame& frame, SyscallArguments const& arguments)
{
    char path[256];
    auto path_length = TRY(copy_string_from_user(arguments.values[0], path, sizeof(path)));

    auto* exec_arguments = new (Base::nothrow) ExecArguments;
    if (!exec_arguments)
        return Error::from_errno(ENOMEM);
    OwnPtr<ExecArguments> owner = adopt_own(*exec_arguments);
    exec_arguments->storage = static_cast<char*>(kmalloc(ExecArguments::storage_size));
    if (!exec_arguments->storage)
        return Error::from_errno(ENOMEM);
    size_t argv_first, argv_count, envp_first, envp_count;
    TRY(exec_arguments->copy_list(arguments.values[1], argv_first, argv_count));
    TRY(exec_arguments->copy_list(arguments.values[2], envp_first, envp_count));

    TRY(exec(StringView(path, path_length), { exec_arguments->strings + argv_first, argv_count },
        { exec_arguments->strings + envp_first, envp_count }, frame));
    // The trap frame now starts the new program; its x0 is overwritten with
    // this result, which the program does not look at.
    return 0;
}

ErrorOr<FlatPtr> Process::sys_waitpid(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    FlatPtr status_address = arguments.values[1];
    int options = static_cast<int>(arguments.values[2]);
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED))
        return Error::from_errno(EINVAL);
    // -1: any child; 0: any in our group; < -1: any in group -pid.
    auto matches = [&](Process const& child) {
        if (pid == -1)
            return true;
        if (pid == 0)
            return child.pgid() == pgid();
        if (pid < 0)
            return child.pgid() == -pid;
        return child.pid() == pid;
    };

    for (;;) {
        // Read before looking, so a change after the look is not missed.
        u64 events = m_child_events.load(MemoryOrder::Acquire);
        RefPtr<Process> reaped;
        ProcessID changed_pid = 0;
        int changed_status = 0;
        bool has_matching_child = false;
        {
            SpinLockGuard guard(process_tree_lock());
            for (auto& child : m_children) {
                if (!matches(child))
                    continue;
                has_matching_child = true;
                if (child.is_zombie()) {
                    reaped = child;
                    m_children.remove(child);
                    child.unregister_from_table();
                    break;
                }
                if (options & WUNTRACED) {
                    if (int signal = child.m_unreported_stop.exchange(0)) {
                        changed_pid = child.pid();
                        changed_status = VULTURE_W_STOPPED(signal);
                        break;
                    }
                }
                if ((options & WCONTINUED) && child.m_unreported_continue.exchange(false)) {
                    changed_pid = child.pid();
                    changed_status = VULTURE_W_CONTINUED;
                    break;
                }
            }
        }
        if (reaped) {
            int status = reaped->m_exit_status;
            ProcessID reaped_pid = reaped->pid();
            // The reference the parent held while the child was listed.
            reaped->unref();
            if (status_address)
                TRY(copy_to_user(status_address, status));
            return static_cast<FlatPtr>(reaped_pid);
        }
        if (changed_pid) {
            if (status_address)
                TRY(copy_to_user(status_address, changed_status));
            return static_cast<FlatPtr>(changed_pid);
        }
        if (!has_matching_child)
            return Error::from_errno(ECHILD);
        if (options & WNOHANG)
            return 0;
        if (has_deliverable_signal())
            return Error::from_errno(EINTR);
        (void)m_child_wait.wait_until_interruptible([&] {
            return m_child_events.load(MemoryOrder::Acquire) != events;
        });
    }
}

ErrorOr<FlatPtr> Process::sys_setpgid(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    auto group = static_cast<ProcessID>(static_cast<i32>(arguments.values[1]));
    if (group < 0)
        return Error::from_errno(EINVAL);
    RefPtr<Process> target = this;
    if (pid != 0 && pid != m_pid) {
        // Only a child of ours.
        target = from_pid(pid);
        if (!target || target->parent_pid() != m_pid)
            return Error::from_errno(ESRCH);
        if (target->sid() != sid())
            return Error::from_errno(EPERM);
    }
    if (group == 0)
        group = target->pid();
    // A session leader stays where it is.
    if (target->sid() == target->pid())
        return Error::from_errno(EPERM);
    // A new group named after the process, or one that exists in this session.
    if (group != target->pid() && !group_exists_in_session(group, sid()))
        return Error::from_errno(EPERM);
    target->m_pgid.store(group);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getpgid(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    if (pid == 0 || pid == m_pid)
        return static_cast<FlatPtr>(pgid());
    auto target = from_pid(pid);
    if (!target)
        return Error::from_errno(ESRCH);
    return static_cast<FlatPtr>(target->pgid());
}

ErrorOr<FlatPtr> Process::sys_setsid(TrapFrame&, SyscallArguments const&)
{
    // A group leader cannot: its group would straddle two sessions.
    if (pgid() == m_pid || group_exists_in_session(m_pid, sid()))
        return Error::from_errno(EPERM);
    m_sid.store(m_pid);
    m_pgid.store(m_pid);
    lose_controlling_terminal();
    return static_cast<FlatPtr>(m_pid);
}

ErrorOr<FlatPtr> Process::sys_getsid(TrapFrame&, SyscallArguments const& arguments)
{
    auto pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    if (pid == 0 || pid == m_pid)
        return static_cast<FlatPtr>(sid());
    auto target = from_pid(pid);
    if (!target)
        return Error::from_errno(ESRCH);
    return static_cast<FlatPtr>(target->sid());
}

}
