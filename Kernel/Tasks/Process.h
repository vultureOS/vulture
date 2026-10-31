/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/AddressSpace.h>
#include <Kernel/Memory/KernelBuffer.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>

namespace Kernel {

class TTY;
using ProcessID = i32;

struct SyscallArguments {
    u64 values[6];
};

struct SignalAction {
    FlatPtr handler { VULTURE_SIG_DFL };
    u64 mask { 0 };
    u64 flags { 0 };
    FlatPtr restorer { 0 };
};

// A running program: an address space, open files, signal state and (for
// now) one thread. Parents hold a reference to each child until they reap
// it with waitpid(); a process whose thread has gone is a zombie until then.
class Process final : public RefCounted<Process> {
public:
    static constexpr size_t max_open_files = 64;

    // Starts `path` from the boot archive as process 1.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Process>> create_init(StringView path, Span<StringView const> arguments);

    // The process of the current thread, if it has one.
    [[nodiscard]] static Process* current();
    [[nodiscard]] static RefPtr<Process> from_pid(ProcessID);
    [[nodiscard]] static size_t count();
    // Every process, living or zombie.
    [[nodiscard]] static Vector<NonnullRefPtr<Process>> all();

    ~Process();

    [[nodiscard]] ProcessID pid() const { return m_pid; }
    [[nodiscard]] ProcessID parent_pid() const;
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    [[nodiscard]] AddressSpace* address_space() { return m_address_space.ptr(); }
    [[nodiscard]] bool is_zombie() const { return m_zombie.load(MemoryOrder::Acquire); }
    // Job control: the process group and session.
    [[nodiscard]] ProcessID pgid() const { return m_pgid.load(); }
    [[nodiscard]] ProcessID sid() const { return m_sid.load(); }
    [[nodiscard]] bool is_stopped() const { return m_stopped.load(); }
    // The controlling terminal, if the session has one.
    [[nodiscard]] TTY* controlling_terminal() const { return m_controlling_terminal.ptr(); }
    // The session leader takes `tty` (which must not belong to another
    // session) for the session.
    ErrorOr<void> set_controlling_terminal(TTY&);
    void lose_controlling_terminal();
    [[nodiscard]] bool is_signal_ignored_or_blocked(int signal) const;
    [[nodiscard]] Credentials const& credentials() const { return m_credentials; }
    [[nodiscard]] u32 umask() const { return m_umask; }

    // Called when the process's thread is gone (its last reference).
    void thread_finished();

    // System calls (see Kernel/API/Syscalls.def).
#define SYSCALL(name, arguments) ErrorOr<FlatPtr> sys_##name(TrapFrame&, SyscallArguments const&);
#include <Kernel/API/Syscalls.def>
#undef SYSCALL

    // A page fault in user mode that the address space could not resolve
    // becomes a signal (or the end of the process).
    void handle_user_fault(TrapFrame&, FlatPtr address, AccessKind, bool permission_fault, PageFaultResult);
    // A fault other than a page fault (illegal instruction, alignment...).
    void handle_user_exception(TrapFrame&, int signal);

    // Delivers one pending signal, on the way back to user mode.
    [[nodiscard]] bool has_deliverable_signal() const;
    void dispatch_signal(TrapFrame&);

    // From any thread: queues `signal` and interrupts sleeps and waits.
    void send_signal(int signal, vulture_siginfo const&);
    // To every process in the group; ESRCH if there is none.
    static ErrorOr<void> send_signal_to_group(ProcessID pgid, int signal, vulture_siginfo const&);
    [[nodiscard]] static bool group_exists_in_session(ProcessID pgid, ProcessID sid);

private:
    friend class Thread;

    Process(ProcessID, Process* parent);

    [[nodiscard]] static ErrorOr<NonnullRefPtr<Process>> create(Process* parent);
    // The process table and the parent's children list each hold a reference.
    void register_with_parent();
    void unregister_from_table();
    ErrorOr<void> exec(StringView path, Span<StringView const> arguments, Span<StringView const> environment, TrapFrame&);
    [[noreturn]] void terminate(int status);
    void report_crash(TrapFrame const&, int signal, vulture_siginfo const&);
    void force_signal(int signal, vulture_siginfo const&);
    void child_state_changed();
    // Job control: stops until SIGCONT (or SIGKILL); resumes a stopped one.
    void stop(int signal);
    void continue_running();
    void notify_parent(int code, int status);

    ErrorOr<int> allocate_fd(NonnullRefPtr<FileDescription>, bool close_on_exec, int lowest = 0);
    ErrorOr<NonnullRefPtr<FileDescription>> file(int fd);
    void set_name(StringView);
    ErrorOr<String> copy_path_from_user(FlatPtr);

    ProcessID m_pid { 0 };
    char m_name[32] {};
    size_t m_name_length { 0 };

    // Parent, children and process table links are guarded by one global
    // lock (see Process.cpp).
    Process* m_parent { nullptr };
    IntrusiveListNode m_sibling_node;
    IntrusiveListNode m_table_node;
    IntrusiveList<Process, &Process::m_sibling_node> m_children;

    OwnPtr<AddressSpace> m_address_space;
    Thread* m_thread { nullptr };
    // The program's ELF file, kept for crash reports.
    RefPtr<KernelBuffer> m_executable;
    // The canonical working directory.
    String m_cwd;

    struct FileDescriptor {
        RefPtr<FileDescription> description;
        bool close_on_exec { false };
    };

    SpinLock m_files_lock;
    FileDescriptor m_files[max_open_files];

    // Signals: pending bits may be set by any thread; the rest is touched
    // only by this process's own thread.
    mutable SpinLock m_signal_lock;
    Atomic<u64> m_pending_signals { 0 };
    u64 m_signal_mask { 0 };
    SignalAction m_signal_actions[NSIG];
    vulture_siginfo m_signal_info[NSIG] {};

    // Job control.
    // sigsuspend(): the mask to restore once its signal has been handled.
    Optional<u64> m_mask_after_suspend;
    WaitQueue m_suspend_wait;
    // alarm(): SIGALRM when it fires.
    Timer m_alarm_timer;
    u64 m_alarm_deadline { 0 };

    Credentials m_credentials;
    u32 m_umask { 022 };
    RefPtr<TTY> m_controlling_terminal;
    Atomic<ProcessID> m_pgid { 0 };
    Atomic<ProcessID> m_sid { 0 };
    Atomic<bool> m_stopped { false };
    WaitQueue m_continue_wait;
    // A stop (its signal) or a continue not yet reported by waitpid().
    Atomic<int> m_unreported_stop { 0 };
    Atomic<bool> m_unreported_continue { false };

    Atomic<bool> m_exiting { false };
    Atomic<bool> m_zombie { false };
    int m_exit_status { 0 };

    // Bumped when a child changes state; waitpid() sleeps on it.
    Atomic<u64> m_child_events { 0 };
    WaitQueue m_child_wait;

public:
    using TableList = IntrusiveList<Process, &Process::m_table_node>;
};

}
