/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

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
    [[nodiscard]] bool has_pending_signal() const;

    // Makes a created thread runnable.
    void start();
    // Waits until the thread has exited and its stack is freed.
    void join();

    [[nodiscard]] u64 id() const { return m_id; }
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    [[nodiscard]] QoS qos() const { return m_qos; }
    [[nodiscard]] State state() const { return m_state.load(MemoryOrder::Relaxed); }
    [[nodiscard]] Optional<u32> pinned_processor() const { return m_pinned_processor; }
    [[nodiscard]] u32 last_processor() const { return m_last_processor.load(MemoryOrder::Relaxed); }
    [[nodiscard]] KernelStack const& stack() const { return m_stack; }
    [[nodiscard]] u64 switch_count() const { return m_switch_count; }

    // User threads belong to a process (null for kernel threads).
    [[nodiscard]] Process* process() const { return m_process.ptr(); }
    void set_process(Process&);
    // Where the registers of user mode are saved on entry to the kernel:
    // the top of the thread's kernel stack.
    [[nodiscard]] TrapFrame& user_trap_frame() const { return *reinterpret_cast<TrapFrame*>(m_stack.top - sizeof(TrapFrame)); }
    // Switches the current processor to this thread's user space.
    void activate_user_space() const;

    ~Thread();

private:
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
    u64 m_switched_in_at { 0 };

public:
    // CPU time so far, including the current run if it is running.
    [[nodiscard]] u64 cpu_ticks() const;
    // Threads waiting for the same thing, linked through the node a thread
    // also uses for its run queue (a thread is never on both).
    using WaitList = IntrusiveList<Thread, &Thread::m_queue_node>;
    using AllThreadsList = IntrusiveList<Thread, &Thread::m_all_threads_node>;
};

}
