/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/AddressSpace.h>
#include <Kernel/API/POSIX/port.h>
#include <Kernel/Memory/KernelBuffer.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>

namespace Kernel {

class Port;
struct PortMessage;

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

// A running program: an address space, open files, signal state and its
// threads. Parents hold a reference to each child until they reap it with
// waitpid(); a process whose threads have gone is a zombie until then.
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
    // For ps and top.
    void describe(struct vulture_process_info&);

    ~Process();

    [[nodiscard]] ProcessID pid() const { return m_pid; }
    [[nodiscard]] ProcessID parent_pid() const;
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    [[nodiscard]] AddressSpace* address_space() { return m_address_space.ptr(); }

    // "debug" on the kernel command line: the kernel log says what every process does.
    static void set_tracing(bool);
    [[nodiscard]] static bool is_tracing();
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

    // Called when one of its threads is gone (its last reference).
    void thread_finished(Thread&);
    // A new thread of this process.
    void add_thread(Thread&);

    // System calls (see Kernel/API/Syscalls.def).
#define SYSCALL(name, arguments) ErrorOr<FlatPtr> sys_##name(TrapFrame&, SyscallArguments const&);
#include <Kernel/API/Syscalls.def>
#undef SYSCALL

    // A page fault in user mode that the address space could not resolve
    // becomes a signal (or the end of the process).
    void handle_user_fault(TrapFrame&, FlatPtr address, AccessKind, bool permission_fault, PageFaultResult);
    // A fault other than a page fault (illegal instruction, alignment...).
    void handle_user_exception(TrapFrame&, int signal);

    // For the current thread, on the way back to user mode: is there
    // anything to do first; do it (deliver one signal, wait while the
    // process is stopped, or end the thread).
    [[nodiscard]] bool has_work_before_user() const;
    void do_work_before_user(TrapFrame&);

    // Should the thread stop waiting and return toward user mode (it has a
    // signal to take, or must end)?
    [[nodiscard]] bool has_deliverable_signal(Thread const&) const;
    // The same for the current thread.
    [[nodiscard]] bool has_deliverable_signal() const;
    void dispatch_signal(TrapFrame&);

    // From any thread: queues `signal` for the process (any thread not
    // blocking it takes it) and interrupts that thread.
    void send_signal(int signal, vulture_siginfo const&);
    // The same for one thread of this process.
    void send_signal_to_thread(Thread&, int signal, vulture_siginfo const&);
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
    void wait_while_stopped(Thread const&);

    // Threads. end_other_threads() makes every other thread end and waits
    // for that; if another thread got there first, the caller ends instead.
    void end_other_threads();
    [[nodiscard]] bool must_end(Thread const&) const;
    [[noreturn]] void exit_current_thread();
    // A thread not blocking `signal` looks at its pending signals.
    void interrupt_a_thread_for(int signal);
    void reroute_pending_signals(u64 signals);
    [[nodiscard]] bool blocked_by_every_thread(int signal);
    [[nodiscard]] String current_directory();

    ErrorOr<int> allocate_fd(NonnullRefPtr<FileDescription>, bool close_on_exec, int lowest = 0);
    void release_fd(int fd);
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

    // Other processes' threads follow these two to describe this one (see describe()):
    // changes to them, and those threads taking a reference, go under this.
    SpinLock m_describe_lock;
    RefPtr<AddressSpace> m_address_space;
    // Its threads, until each ends. Lock order: m_signal_lock, then this.
    mutable SpinLock m_threads_lock;
    Thread::ProcessThreadList m_threads;
    // Thread objects not destroyed yet: threads that left the list may
    // still be on a processor, on this address space.
    Atomic<u32> m_thread_objects { 0 };
    WaitQueue m_thread_gone_wait;
    // The thread ending all the others (for exit or exec).
    Atomic<Thread*> m_ending_thread { nullptr };
    // The program's ELF file, kept for crash reports.
    // Ports (Syscalls/PortCalls.cpp).
    ErrorOr<NonnullOwnPtr<PortMessage>> build_port_message(FlatPtr user_message, bool& move_rights, Vector<int>& sent_fds, Vector<vulture_port_region>& sent_regions);
    void finish_move(bool move_rights, Vector<int> const& sent_fds, Vector<vulture_port_region> const& sent_regions);
    ErrorOr<void> deliver_port_message(Port&, bool nonblocking, FlatPtr user_received);

    RefPtr<KernelBuffer> m_executable;
    FlatPtr m_executable_bias { 0 };
    // The dynamic linker the program runs with, if any.
    RefPtr<KernelBuffer> m_interpreter;
    FlatPtr m_interpreter_bias { 0 };
    // The canonical working directory.
    Mutex m_cwd_lock;
    String m_cwd;
    i64 m_start_time { 0 };
    Atomic<u64> m_exited_cpu_ticks { 0 };
    // exec() of a "#!" script nests at most this deep.
    u32 m_interpreter_depth { 0 };

    struct FileDescriptor {
        RefPtr<FileDescription> description;
        bool close_on_exec { false };
    };

    SpinLock m_files_lock;
    FileDescriptor m_files[max_open_files];

    // Signals sent to the process and what each does; masks, and signals
    // sent to one thread, are the thread's (see Thread.h).
    mutable SpinLock m_signal_lock;
    Atomic<u64> m_pending_signals { 0 };
    SignalAction m_signal_actions[NSIG];
    vulture_siginfo m_signal_info[NSIG] {};

    // Job control.
    // sigsuspend() and sigtimedwait() wait here.
    WaitQueue m_suspend_wait;
    // alarm(): SIGALRM when it fires.
    Mutex m_alarm_lock;
    Timer m_alarm_timer;
    u64 m_alarm_deadline { 0 };

    Credentials m_credentials;
    u32 m_umask { 022 };
    RefPtr<TTY> m_controlling_terminal;
    // A send right to the name service (vlaunchd), inherited across fork
    // and exec, as a Mach task's bootstrap port is.
    RefPtr<FileDescription> m_bootstrap_port;
    Atomic<ProcessID> m_pgid { 0 };
    Atomic<ProcessID> m_sid { 0 };
    Atomic<bool> m_stopped { false };
    WaitQueue m_continue_wait;
    // A stop (its signal) or a continue not yet reported by waitpid().
    Atomic<int> m_unreported_stop { 0 };
    Atomic<bool> m_unreported_continue { false };

    Atomic<bool> m_zombie { false };
    int m_exit_status { 0 };

    // Bumped when a child changes state; waitpid() sleeps on it.
    Atomic<u64> m_child_events { 0 };
    WaitQueue m_child_wait;

public:
    using TableList = IntrusiveList<Process, &Process::m_table_node>;
};

}
