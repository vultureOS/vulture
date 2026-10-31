/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Time/Time.h>

// Lock order: a wait list's lock, then a run queue's lock. A processor
// holds at most one run queue lock at a time.
//
// The one subtle rule: a thread is "on" a processor from the moment it is
// picked until that processor has finished saving it (finish_switch). A
// thread that blocks may be woken, and one that yields may be stolen, before
// then. So wakers wait for the save to finish before queueing the thread,
// and stealing skips threads still being saved; a processor never has to
// wait for another in the middle of a switch.

namespace Kernel {

Atomic<bool> Scheduler::s_running { false };

void RunQueue::push(Thread& thread)
{
    VERIFY(lock.is_locked_by_current_processor());
    m_classes[to_underlying(thread.qos())].append(thread);
    m_count.fetch_add(1, MemoryOrder::Relaxed);
    if (!thread.m_pinned_processor.has_value())
        m_stealable_count.fetch_add(1, MemoryOrder::Relaxed);
}

void RunQueue::remove(Thread& thread)
{
    m_classes[to_underlying(thread.qos())].remove(thread);
    m_count.fetch_sub(1, MemoryOrder::Relaxed);
    if (!thread.m_pinned_processor.has_value())
        m_stealable_count.fetch_sub(1, MemoryOrder::Relaxed);
}

Thread* RunQueue::pop_most_urgent()
{
    VERIFY(lock.is_locked_by_current_processor());
    for (size_t index = qos_class_count; index-- > 0;) {
        if (auto* thread = m_classes[index].first()) {
            remove(*thread);
            return thread;
        }
    }
    return nullptr;
}

Thread* RunQueue::steal_for(u32 processor_id)
{
    VERIFY(lock.is_locked_by_current_processor());
    for (size_t index = qos_class_count; index-- > 0;) {
        // The last eligible thread: the one that waited least, so it is
        // the least likely to still have its data in this processor's cache.
        Thread* candidate = nullptr;
        for (auto& thread : m_classes[index]) {
            bool allowed = !thread.m_pinned_processor.has_value() || thread.m_pinned_processor.value() == processor_id;
            if (allowed && !thread.m_on_processor.load(MemoryOrder::Acquire))
                candidate = &thread;
        }
        if (candidate) {
            remove(*candidate);
            return candidate;
        }
    }
    return nullptr;
}

u8 RunQueue::most_urgent_rank() const
{
    VERIFY(lock.is_locked_by_current_processor());
    for (size_t index = qos_class_count; index-- > 0;) {
        if (!m_classes[index].is_empty())
            return Scheduler::rank(static_cast<QoS>(index));
    }
    return 0;
}

void Scheduler::initialize()
{
    auto& processor = Processor::current();
    // The global thread list keeps both threads alive.
    auto boot = Thread::adopt_current_context("boot"_sv, { processor.stack_bottom(), processor.stack_top() }, { .qos = QoS::Default, .pinned_processor = processor.id() });
    auto idle = Thread::create("idle-0"_sv, idle_thread_main, nullptr, { .qos = QoS::Background, .pinned_processor = processor.id() });
    if (idle.is_error())
        panic("Scheduler: cannot create the idle thread"_sv);

    InterruptDisabler disabler;
    processor.m_current_thread = boot.ptr();
    processor.m_idle_thread = idle.value().ptr();
    processor.m_running_rank.store(rank(boot->qos()));
    s_running.store(true, MemoryOrder::Release);
}

void Scheduler::run_secondary_processor()
{
    auto& processor = Processor::current();
    FixedStringBuilder<16> name;
    format_to(name, "idle-{}", processor.id());
    auto idle = Thread::adopt_current_context(name.view(), { processor.stack_bottom(), processor.stack_top() }, { .qos = QoS::Background, .pinned_processor = processor.id() });
    processor.m_current_thread = idle.ptr();
    processor.m_idle_thread = idle.ptr();
    processor.m_running_rank.store(0);

    processor.m_online.store(true, MemoryOrder::Release);
    Processor::s_online_count.fetch_add(1, MemoryOrder::AcquireRelease);
    Arch::enable_interrupts();
    idle_loop();
}

void Scheduler::idle_thread_main(void*)
{
    idle_loop();
}

void Scheduler::idle_loop()
{
    for (;;) {
        // Masked while looking for work: an IPI arriving after the check
        // still ends the WFI, which returns for pending interrupts.
        auto state = Arch::disable_interrupts();
        auto& processor = Processor::current();
        bool work_elsewhere = false;
        for (u32 id = 0; id < Processor::count() && !work_elsewhere; ++id)
            work_elsewhere = id != processor.id() && Processor::with_id(id).run_queue().stealable_count() > 0;
        if (processor.m_need_reschedule || processor.m_run_queue.count() > 0 || work_elsewhere)
            schedule();
        else
            Arch::wait_for_interrupt();
        Arch::restore_interrupts(state);
    }
}

// Arms the time slice while others wait to run here; cancels it otherwise.
// A lone thread runs without being interrupted.
void Scheduler::update_time_slice(Processor& processor)
{
    auto& slice = processor.m_time_slice;
    bool needed = processor.m_current_thread != processor.m_idle_thread && processor.m_run_queue.count() > 0;
    if (!needed) {
        if (slice.is_pending())
            processor.m_timer_queue.cancel(slice);
        processor.m_time_slice_armed.store(false, MemoryOrder::Relaxed);
        return;
    }
    if (slice.is_pending())
        return;
    processor.m_timer_queue.add(slice, Time::now() + Time::ticks_from_microseconds(time_slice_microseconds), [](Timer&) {
        auto& current = Processor::current();
        current.m_time_slice_armed.store(false, MemoryOrder::Relaxed);
        current.m_need_reschedule = true;
    });
    processor.m_time_slice_armed.store(true, MemoryOrder::Relaxed);
}

void Scheduler::schedule()
{
    VERIFY(!Arch::are_interrupts_enabled());
    auto& processor = Processor::current();
    auto& current = *processor.m_current_thread;
    bool current_is_idle = &current == processor.m_idle_thread;
    processor.m_need_reschedule = false;

    Thread* next = nullptr;
    {
        SpinLockGuard guard(processor.m_run_queue.lock);
        // Still runnable: to the back of its class, so others take turns.
        if (!current_is_idle && current.state() == Thread::State::Running) {
            current.m_state.store(Thread::State::Runnable, MemoryOrder::Relaxed);
            processor.m_run_queue.push(current);
        }
        next = processor.m_run_queue.pop_most_urgent();
    }
    if (!next)
        next = steal_work(processor);
    if (!next)
        next = processor.m_idle_thread;

    if (next == &current) {
        current.m_state.store(Thread::State::Running, MemoryOrder::Relaxed);
        update_time_slice(processor);
        return;
    }
    switch_to(processor, current, *next);
}

void Scheduler::switch_to(Processor& processor, Thread& current, Thread& next)
{
    // Wakers and thieves never hand over a thread still being saved.
    VERIFY(!next.m_on_processor.load(MemoryOrder::Acquire));
    next.m_on_processor.store(true, MemoryOrder::Relaxed);
    next.m_state.store(Thread::State::Running, MemoryOrder::Relaxed);
    next.m_last_processor.store(processor.id(), MemoryOrder::Relaxed);
    ++next.m_switch_count;

    processor.m_current_thread = &next;
    processor.m_switching_from = &current;
    processor.m_running_rank.store(&next == processor.m_idle_thread ? 0 : rank(next.qos()), MemoryOrder::Relaxed);
    ++processor.m_context_switches;
    // A fresh slice for the thread coming in.
    if (processor.m_time_slice.is_pending())
        processor.m_timer_queue.cancel(processor.m_time_slice);
    update_time_slice(processor);

    Arch::switch_context(current.m_context, next.m_context);

    // Running `current` again, possibly on another processor.
    finish_switch();
}

void Scheduler::finish_switch()
{
    auto& processor = Processor::current();
    Thread* previous = exchange(processor.m_switching_from, nullptr);
    if (!previous)
        return;
    bool dying = previous->state() == Thread::State::Dying;
    // From here another processor may resume `previous` (unless it is dying).
    previous->m_on_processor.store(false, MemoryOrder::Release);
    if (dying)
        reap(*previous);
}

// Runs on the next thread's stack, so the dead thread's stack can go.
void Scheduler::reap(Thread& thread)
{
    thread.release_stack();
    thread.unregister_thread();

    Thread::WaitList joiners;
    {
        SpinLockGuard guard(thread.m_exit_lock);
        thread.m_state.store(Thread::State::Dead, MemoryOrder::Release);
        while (auto* joiner = thread.m_joiners.take_first())
            joiners.append(*joiner);
    }
    while (auto* joiner = joiners.take_first())
        wake(*joiner);
    thread.unref();
}

Thread* Scheduler::steal_work(Processor& thief)
{
    u32 count = Processor::count();
    for (u32 offset = 1; offset < count; ++offset) {
        auto& victim = Processor::with_id((thief.id() + offset) % count);
        if (!victim.is_online() || victim.m_run_queue.stealable_count() == 0)
            continue;
        SpinLockGuard guard(victim.m_run_queue.lock);
        if (auto* thread = victim.m_run_queue.steal_for(thief.id()))
            return thread;
    }
    return nullptr;
}

// An idle processor if there is one, starting with the thread's last (its
// cache may still hold the thread's data); otherwise the last one.
Processor& Scheduler::choose_processor(Thread& thread)
{
    if (thread.m_pinned_processor.has_value())
        return Processor::with_id(thread.m_pinned_processor.value());
    u32 count = Processor::count();
    u32 last = thread.last_processor() < count ? thread.last_processor() : 0;
    for (u32 offset = 0; offset < count; ++offset) {
        auto& candidate = Processor::with_id((last + offset) % count);
        if (candidate.is_online() && candidate.m_running_rank.load(MemoryOrder::Relaxed) == 0 && candidate.m_run_queue.count() == 0)
            return candidate;
    }
    auto& fallback = Processor::with_id(last);
    return fallback.is_online() ? fallback : Processor::current();
}

void Scheduler::notify(Processor& target, Thread& thread)
{
    u8 thread_rank = rank(thread.qos());
    auto& self = Processor::current();
    if (&target == &self) {
        if (thread_rank > self.m_running_rank.load(MemoryOrder::Relaxed))
            self.m_need_reschedule = true;
        else
            update_time_slice(self);
        return;
    }
    // The target only needs telling if it must switch now, or arm its slice.
    if (thread_rank > target.m_running_rank.load(MemoryOrder::Relaxed) || !target.m_time_slice_armed.load(MemoryOrder::Relaxed))
        Arch::send_ipi(target.hardware_id(), IPI::Wakeup);
}

void Scheduler::enqueue(Thread& thread)
{
    InterruptDisabler disabler;
    auto& target = choose_processor(thread);
    {
        SpinLockGuard guard(target.m_run_queue.lock);
        thread.m_state.store(Thread::State::Runnable, MemoryOrder::Release);
        target.m_run_queue.push(thread);
    }
    notify(target, thread);
}

void Scheduler::wake(Thread& thread)
{
    VERIFY(thread.state() == Thread::State::Blocked);
    // It may still be switching away on its processor; it cannot be queued
    // (and resumed elsewhere) before that finishes.
    while (thread.m_on_processor.load(MemoryOrder::Acquire))
        Arch::pause();
    enqueue(thread);
}

void Scheduler::block(Thread::WaitList* waiters, SpinLock* lock, Arch::InterruptState state)
{
    VERIFY(!Arch::are_interrupts_enabled());
    auto& processor = Processor::current();
    VERIFY(!processor.is_in_interrupt());
    auto& thread = *processor.m_current_thread;
    VERIFY(&thread != processor.m_idle_thread);

    thread.m_state.store(Thread::State::Blocked, MemoryOrder::Release);
    if (waiters)
        waiters->append(thread);
    if (lock)
        lock->unlock_keeping_interrupts_masked();
    schedule();
    Arch::restore_interrupts(state);
}

void Scheduler::yield()
{
    auto state = Arch::disable_interrupts();
    schedule();
    Arch::restore_interrupts(state);
}

void Scheduler::preempt_if_needed()
{
    if (!is_running())
        return;
    auto& processor = Processor::current();
    if (processor.m_need_reschedule && !processor.is_in_interrupt())
        schedule();
}

void Scheduler::reschedule_if_needed()
{
    if (!is_running() || !Arch::are_interrupts_enabled())
        return;
    auto state = Arch::disable_interrupts();
    if (Processor::current().m_need_reschedule)
        schedule();
    Arch::restore_interrupts(state);
}

void Scheduler::handle_wakeup_ipi()
{
    auto& processor = Processor::current();
    u8 queued_rank;
    {
        SpinLockGuard guard(processor.m_run_queue.lock);
        queued_rank = processor.m_run_queue.most_urgent_rank();
    }
    if (queued_rank > processor.m_running_rank.load(MemoryOrder::Relaxed))
        processor.m_need_reschedule = true;
    else
        update_time_slice(processor);
}

}

extern "C" void kernel_thread_start(Kernel::Arch::ThreadEntry entry, void* argument)
{
    Kernel::Scheduler::finish_switch();
    Kernel::Arch::enable_interrupts();
    entry(argument);
    Kernel::Thread::exit();
}
