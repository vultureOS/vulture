/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/ThreadContext.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Time/TimerQueue.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/Optional.h>
#include <LibBase/RefPtr.h>
#include <LibBase/StringView.h>

namespace Kernel {

class Process;
class RunQueue;
class WaitQueue;

// Quality of service: what a thread's work is for, from least to most
// urgent. A runnable thread of a higher class always runs before one of a
// lower class; threads of one class share processors in turns.
enum class QoS : u8 {
    Background, // Maintenance the user does not wait for.
    Utility, // Long-running work with visible progress.
    Default,
    UserInitiated, // Work the user started and is waiting for.
    UserInteractive, // Input, animation, drawing.
};

constexpr size_t qos_class_count = 5;

StringView qos_name(QoS);

using ThreadEntry = Arch::ThreadEntry;

struct ThreadOptions {
    QoS qos { QoS::Default };
    // Run only on this processor.
    Optional<u32> pinned_processor {};
};

// A kernel thread: its own stack and saved CPU state, scheduled on any
// processor (or one it is pinned to). Owned by references: the creator gets
// one, and the scheduler keeps one until the thread has exited.
class Thread final : public RefCounted<Thread> {
public:
    enum class State : u8 {
        Created, // Not started yet.
        Runnable, // In a run queue.
        Running, // On a processor.
        Blocked, // Waiting to be woken.
        Dying, // Exited; the next thread on its processor reaps it.
        Dead, // Stack freed; only references remain.
    };

    [[nodiscard]] static ErrorOr<NonnullRefPtr<Thread>> create(StringView name, ThreadEntry, void* argument, ThreadOptions = {});

    // Turns the code already running on this processor into a thread, on
    // the stack it is using. For boot and idle threads.
    [[nodiscard]] static NonnullRefPtr<Thread> adopt_current_context(StringView name, KernelStack, ThreadOptions);

    [[nodiscard]] static Thread& current();

    // Ends the current thread.
    [[noreturn]] static void exit();
    // Gives the processor to another thread of the same or a higher class,
    // if one is runnable.
    static void yield();

    enum class SleepResult : u8 {
        Slept,
        Interrupted, // By interrupt_sleep(), such as for a signal.
    };
    static SleepResult sleep_until(u64 deadline);
    static SleepResult sleep_for_microseconds(u64);
    // Ends the thread's current sleep early. (A process thread also does not
    // start a sleep while a signal is pending.)
    void interrupt_sleep();

    // Interruptible waits: the queue a thread waits on, so a signal can
    // wake it there (see WaitQueue::wait_until_interruptible()).
    void set_blocked_on(WaitQueue*);
    void interrupt_waits();
    // For a signal: ends its sleep or wait, or makes it enter the kernel if
    // it is running.
    void interrupt_for_signal();
    [[nodiscard]] bool has_pending_signal() const;

    // Makes a created thread runnable.
    void start();
    // Waits until the thread has exited and its stack is freed.
    void join();

    [[nodiscard]] u64 id() const { return m_id; }
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    // How urgent it is now. Which is what it was asked to be, or more, if something more urgent is waiting for it.
    [[nodiscard]] QoS qos() const { return static_cast<QoS>(m_effective_qos.load(MemoryOrder::Acquire)); }
    [[nodiscard]] QoS base_qos() const { return m_qos; }
    void set_base_qos(QoS);
    // `key` says who's asking: a lock, or someone waiting for one. At least this...
    void raise_boost(void const* key, QoS);
    // ...or exactly this, as that's changed...
    void set_boost(void const* key, QoS);
    // ...and that's over. There's room for so many at once, and those that don't fit aren't anything worse than a slower thread.
    void remove_boost(void const* key);
    // A message for it, from someone this urgent; until it goes back for another. Background is no boost.
    void set_message_boost(QoS);
    void raise_message_boost(QoS);
    // While it's waiting for this thread: which then has to go as fast as this one does, as it changes.
    void set_boost_target(Thread*);
    // pthread_cancel()'s.
    void request_cancel() { m_cancel_requested.store(true); }
    void set_cancel_enabled(bool on) { m_cancel_enabled.store(on); }
    void set_cancel_async(bool on) { m_cancel_async.store(on); }
    void set_in_cancellation_point(bool on) { m_in_cancellation_point.store(on); }
    [[nodiscard]] bool cancellation_is_due() const { return m_cancel_requested.load() && m_cancel_enabled.load() && (m_in_cancellation_point.load() || m_cancel_async.load()); }
    [[nodiscard]] State state() const { return m_state.load(MemoryOrder::Relaxed); }
    [[nodiscard]] Optional<u32> pinned_processor() const { return m_pinned_processor; }
    [[nodiscard]] u32 last_processor() const { return m_last_processor.load(MemoryOrder::Relaxed); }
    [[nodiscard]] KernelStack const& stack() const { return m_stack; }
    [[nodiscard]] u64 switch_count() const { return m_switch_count; }

    // User threads belong to a process (null for kernel threads).
    [[nodiscard]] Process* process() const { return m_process.ptr(); }
    // Its tracer's asked for one instruction at a time.
    [[nodiscard]] bool is_single_stepping() const { return m_single_stepping.load(); }
    void set_single_stepping(bool on) { m_single_stepping.store(on); }
    // A signal interrupted this system call: it may yet be made again.
    void note_syscall_interrupted(u64 number, u64 first_argument)
    {
        m_syscall_interrupted = true;
        m_interrupted_syscall = number;
        m_interrupted_syscall_argument = first_argument;
    }
    void set_process(Process&);
    // Where the registers of user mode are saved on entry to the kernel:
    // the top of the thread's kernel stack.
    [[nodiscard]] TrapFrame& user_trap_frame() const { return *reinterpret_cast<TrapFrame*>(m_stack.top - sizeof(TrapFrame)); }
    // Switches the current processor to this thread's user space.
    void activate_user_space() const;

    ~Thread();

private:
    friend class Process;
    friend class Scheduler;
    friend class RunQueue;
    friend class RefCounted<Thread>;

    Thread(StringView name, ThreadOptions);

    void register_thread();
    void unregister_thread();
    void release_stack();

    u64 m_id { 0 };
    char m_name[32] {};
    size_t m_name_length { 0 };
    QoS m_qos { QoS::Default };
    Atomic<u8> m_effective_qos { to_underlying(QoS::Default) };
    // Which run queue it's on, and in which class there: under that queue's lock.
    Atomic<RunQueue*> m_run_queue { nullptr };
    u8 m_queued_class { 0 };
    struct Boost {
        void const* key { nullptr };
        QoS qos { QoS::Background };
    };
    static constexpr size_t max_boosts = 8;
    SpinLock m_boost_lock;
    Boost m_boosts[max_boosts];
    Atomic<u32> m_boost_count { 0 };
    QoS m_message_boost { QoS::Background };
    RefPtr<Thread> m_boost_target;
    void update_effective_qos();
    Atomic<bool> m_cancel_requested { false };
    Atomic<bool> m_cancel_enabled { true };
    Atomic<bool> m_cancel_async { false };
    Atomic<bool> m_in_cancellation_point { false };
    Optional<u32> m_pinned_processor;

    Atomic<State> m_state { State::Created };
    // True from the moment a processor picks the thread until it has
    // finished switching away from it. No other processor may resume the
    // thread meanwhile, even if it is already runnable again.
    Atomic<bool> m_on_processor { false };
    Atomic<u32> m_last_processor { 0 };

    KernelStack m_stack;
    bool m_owns_stack { false };
    Arch::ThreadContext m_context;

    IntrusiveListNode m_queue_node;
    IntrusiveListNode m_all_threads_node;
    Timer m_sleep_timer;

    SpinLock m_exit_lock;
    IntrusiveList<Thread, &Thread::m_queue_node> m_joiners;

    RefPtr<Process> m_process;
    // In its process's list of threads, until it ends.
    IntrusiveListNode m_process_node;

    // A user thread's own part of the signal state (the rest is its
    // process's). Only the thread itself changes its mask.
    Atomic<u64> m_signal_mask { 0 };
    // Directed at this thread: its faults, pthread_kill().
    Atomic<u64> m_pending_signals { 0 };
    vulture_siginfo m_signal_info[NSIG] {};
    // sigsuspend(): the mask to restore once its signal has been handled.
    Optional<u64> m_mask_after_suspend;
    // sigaltstack(); no size means there is none.
    FlatPtr m_alternate_stack { 0 };
    size_t m_alternate_stack_size { 0 };
    // Cleared and woken as a futex when the thread ends (pthread_join).
    FlatPtr m_clear_tid_address { 0 };

    // Guards m_blocked_on, so a waker never uses a queue that is gone.
    SpinLock m_blocked_lock;
    WaitQueue* m_blocked_on { nullptr };

    // Sleeping: whoever finds m_sleeping set (under the lock) wakes the thread.
    SpinLock m_sleep_lock;
    bool m_sleeping { false };
    bool m_sleep_interrupted { false };

    u64 m_switch_count { 0 };
    // CPU time used, in counter ticks; and when it last got a processor.
    Atomic<u64> m_cpu_ticks { 0 };
    Atomic<bool> m_single_stepping { false };
    // A system call that a signal interrupted, and that may yet be made again: see Syscalls/Dispatch.cpp.
    bool m_syscall_interrupted { false };
    u64 m_interrupted_syscall { 0 };
    u64 m_interrupted_syscall_argument { 0 };
    u64 m_switched_in_at { 0 };

public:
    // CPU time so far, including the current run if it is running.
    [[nodiscard]] u64 cpu_ticks() const;
    // Threads waiting for the same thing, linked through the node a thread
    // also uses for its run queue (a thread is never on both).
    using WaitList = IntrusiveList<Thread, &Thread::m_queue_node>;
    using AllThreadsList = IntrusiveList<Thread, &Thread::m_all_threads_node>;
    using ProcessThreadList = IntrusiveList<Thread, &Thread::m_process_node>;
};

}
