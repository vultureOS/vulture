/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/Panic.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel {

static Atomic<u64> s_next_thread_id { 1 };
static SpinLock s_all_threads_lock;
static Thread::AllThreadsList s_all_threads;

StringView qos_name(QoS qos)
{
    switch (qos) {
    case QoS::Background:
        return "background"_sv;
    case QoS::Utility:
        return "utility"_sv;
    case QoS::Default:
        return "default"_sv;
    case QoS::UserInitiated:
        return "user-initiated"_sv;
    case QoS::UserInteractive:
        return "user-interactive"_sv;
    }
    return "unknown"_sv;
}

Thread::Thread(StringView name, ThreadOptions options)
    : m_id(s_next_thread_id.fetch_add(1, MemoryOrder::Relaxed))
    , m_qos(options.qos)
    , m_pinned_processor(options.pinned_processor)
{
    m_name_length = min(name.length(), sizeof(m_name));
    __builtin_memcpy(m_name, name.characters_without_null_termination(), m_name_length);
    m_sleep_timer.context = this;
    if (m_pinned_processor.has_value())
        VERIFY(m_pinned_processor.value() < Processor::count());
}

Thread::~Thread()
{
    VERIFY(state() == State::Dead || state() == State::Created);
    release_stack();
    if (m_process)
        m_process->thread_finished(*this);
}

void Thread::set_process(Process& process)
{
    VERIFY(!m_process);
    m_process = process;
    process.add_thread(*this);
}

void Thread::activate_user_space() const
{
    if (m_process && m_process->address_space())
        m_process->address_space()->activate();
    else
        Arch::activate_user_page_table(nullptr, 0);
}

void Thread::release_stack()
{
    if (m_owns_stack && m_stack.top) {
        memory_manager().free_kernel_stack(m_stack);
        m_stack = {};
    }
}

// The scheduler's reference, held until the thread is reaped.
void Thread::register_thread()
{
    ref();
    SpinLockGuard guard(s_all_threads_lock);
    s_all_threads.append(*this);
}

void Thread::unregister_thread()
{
    SpinLockGuard guard(s_all_threads_lock);
    s_all_threads.remove(*this);
}

ErrorOr<NonnullRefPtr<Thread>> Thread::create(StringView name, ThreadEntry entry, void* argument, ThreadOptions options)
{
    auto stack = TRY(memory_manager().allocate_kernel_stack());
    auto* thread = new (Base::nothrow) Thread(name, options);
    if (!thread) {
        memory_manager().free_kernel_stack(stack);
        return Error::from_string_literal("out of memory");
    }
    thread->m_stack = stack;
    thread->m_owns_stack = true;
    // The top of the stack is kept for the user trap frame.
    Arch::initialize_thread_context(thread->m_context, stack.top - sizeof(TrapFrame), entry, argument);
    auto reference = adopt_ref(*thread);
    thread->register_thread();
    return reference;
}

NonnullRefPtr<Thread> Thread::adopt_current_context(StringView name, KernelStack stack, ThreadOptions options)
{
    auto* thread = new Thread(name, options);
    thread->m_stack = stack;
    thread->m_state.store(State::Running);
    thread->m_on_processor.store(true);
    thread->m_last_processor.store(Processor::current().id());
    auto reference = adopt_ref(*thread);
    thread->register_thread();
    return reference;
}

Thread& Thread::current()
{
    // Read with interrupts masked: otherwise the thread could move to
    // another processor between finding the processor and reading it.
    InterruptDisabler disabler;
    auto* thread = Processor::current().current_thread();
    VERIFY(thread);
    return *thread;
}

void Thread::start()
{
    VERIFY(state() == State::Created);
    // A hint for placement: start near the creator.
    {
        InterruptDisabler disabler;
        m_last_processor.store(Processor::current().id(), MemoryOrder::Relaxed);
    }
    Scheduler::enqueue(*this);
    Scheduler::reschedule_if_needed();
}

void Thread::join()
{
    VERIFY(this != &current());
    auto state = m_exit_lock.lock();
    if (this->state() == State::Dead) {
        m_exit_lock.unlock(state);
        return;
    }
    Scheduler::block(&m_joiners, &m_exit_lock, state);
}

void Thread::exit()
{
    [[maybe_unused]] auto state = Arch::disable_interrupts();
    current().m_state.store(State::Dying, MemoryOrder::Release);
    Scheduler::yield();
    VERIFY_NOT_REACHED();
}

void Thread::yield()
{
    Scheduler::yield();
}

Thread::SleepResult Thread::sleep_until(u64 deadline)
{
    VERIFY(Arch::are_interrupts_enabled());
    auto& thread = current();
    auto state = thread.m_sleep_lock.lock();
    // A signal sent before this point is seen here; one sent after finds
    // m_sleeping set (both under the lock), so none is missed.
    if (thread.m_process && thread.m_process->has_deliverable_signal(thread)) {
        thread.m_sleep_lock.unlock(state);
        return SleepResult::Interrupted;
    }
    if (Time::now() >= deadline) {
        thread.m_sleep_lock.unlock(state);
        return SleepResult::Slept;
    }
    // The timer fires on this processor once the switch away is complete
    // (it needs interrupts, which stay masked until then).
    thread.m_sleeping = true;
    Processor::current().timer_queue().add(thread.m_sleep_timer, deadline, [](Timer& timer) {
        auto& sleeper = *static_cast<Thread*>(timer.context);
        auto timer_state = sleeper.m_sleep_lock.lock();
        bool wake = exchange(sleeper.m_sleeping, false);
        sleeper.m_sleep_lock.unlock(timer_state);
        if (wake)
            Scheduler::wake(sleeper);
    });
    Scheduler::block(nullptr, &thread.m_sleep_lock, state);

    // Woken by the timer or interrupted. Make sure the timer is not still
    // pending, or running, before this thread can go away.
    thread.m_sleep_timer.cancel();
    SpinLockGuard guard(thread.m_sleep_lock);
    return exchange(thread.m_sleep_interrupted, false) ? SleepResult::Interrupted : SleepResult::Slept;
}

void Thread::set_blocked_on(WaitQueue* queue)
{
    SpinLockGuard guard(m_blocked_lock);
    m_blocked_on = queue;
}

void Thread::interrupt_waits()
{
    SpinLockGuard guard(m_blocked_lock);
    if (m_blocked_on)
        m_blocked_on->wake_all();
}

bool Thread::has_pending_signal() const
{
    return m_process && m_process->has_deliverable_signal(*this);
}

void Thread::interrupt_for_signal()
{
    interrupt_sleep();
    // A waitpid(), or a wait on a pipe, returns to let the signal through.
    interrupt_waits();
    Scheduler::kick(*this);
}

void Thread::interrupt_sleep()
{
    auto state = m_sleep_lock.lock();
    bool wake = exchange(m_sleeping, false);
    if (wake)
        m_sleep_interrupted = true;
    m_sleep_lock.unlock(state);
    if (wake)
        Scheduler::wake(*this);
}

Thread::SleepResult Thread::sleep_for_microseconds(u64 microseconds)
{
    return sleep_until(Time::now() + Time::ticks_from_microseconds(microseconds));
}

u64 Thread::cpu_ticks() const
{
    u64 total = m_cpu_ticks.load(MemoryOrder::Relaxed);
    if (m_state.load(MemoryOrder::Relaxed) == State::Running)
        total += Time::now() - m_switched_in_at;
    return total;
}

}
