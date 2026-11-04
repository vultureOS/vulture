/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/thread.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/API/POSIX/wait.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/NumericLimits.h>

namespace Kernel {

namespace {

// Sleepers on futexes, hashed by address. A waiter joins its bucket before
// it reads the word: reading may fault, and sleep, so no lock can be held
// for it. A wake that comes after the waiter read the word finds it in the
// bucket; one that comes before changed the word first, which the waiter
// then sees. Each waiter has its own wait queue, so a wake wakes exactly
// the ones it chose.
//
// What's waited for is an address in an address space; or, in memory that's shared, a place in the file it's the memory of, so that
// everyone mapping that's waiting for the same thing.
struct FutexKey {
    void const* where { nullptr };
    u64 offset { 0 };
    [[nodiscard]] bool operator==(FutexKey const&) const = default;
};

struct FutexWaiter {
    IntrusiveListNode node;
    FutexKey key;
    Atomic<bool> woken { false };
    WaitQueue queue;
};

struct FutexBucket {
    SpinLock lock;
    IntrusiveList<FutexWaiter, &FutexWaiter::node> waiters;
};

constexpr size_t bucket_count = 256;
FutexBucket s_buckets[bucket_count];

FutexKey key_for(AddressSpace& space, FlatPtr address)
{
    if (auto location = space.shared_location(address); location.has_value())
        return { location->file, location->offset };
    return { &space, address };
}

FutexBucket& bucket_for(FutexKey const& futex)
{
    FlatPtr key = reinterpret_cast<FlatPtr>(futex.where) ^ (futex.offset >> 2) ^ (futex.offset >> 12);
    return s_buckets[(key ^ (key >> 8)) % bucket_count];
}

size_t futex_wake(FutexKey const& key, size_t count)
{
    auto& bucket = bucket_for(key);
    size_t woken = 0;
    SpinLockGuard guard(bucket.lock);
    // A woken waiter leaves the list here: it takes this lock before it goes
    // away, so it is still there while we wake it.
    for (auto iterator = bucket.waiters.begin(); iterator != bucket.waiters.end() && woken < count;) {
        auto& waiter = *iterator;
        ++iterator;
        if (waiter.key != key)
            continue;
        bucket.waiters.remove(waiter);
        waiter.woken.store(true);
        waiter.queue.wake_all();
        ++woken;
    }
    return woken;
}

}

ErrorOr<FlatPtr> Process::sys_futex(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr address = arguments.values[0];
    int operation = static_cast<int>(arguments.values[1]);
    u32 value = static_cast<u32>(arguments.values[2]);
    if (address % sizeof(u32) != 0 || !is_user_range(address, sizeof(u32)))
        return Error::from_errno(EINVAL);
    auto const key = key_for(*m_address_space, address);

    if (operation == VULTURE_FUTEX_WAKE)
        return static_cast<FlatPtr>(futex_wake(key, value));
    if (operation != VULTURE_FUTEX_WAIT && operation != VULTURE_FUTEX_WAIT_FOR_HOLDER && operation != VULTURE_FUTEX_WAIT_CANCELLABLE)
        return Error::from_errno(EINVAL);

    auto& self = Thread::current();
    bool const cancellable = operation == VULTURE_FUTEX_WAIT_CANCELLABLE;
    if (cancellable) {
        self.set_in_cancellation_point(true);
        if (self.cancellation_is_due()) {
            self.set_in_cancellation_point(false);
            return Error::from_errno(ECANCELED);
        }
    }

    // Someone in this process that's in the way: they've got to get on with it, and this waits with them for as long as that takes.
    RefPtr<Thread> holder;
    if (operation == VULTURE_FUTEX_WAIT_FOR_HOLDER && arguments.values[4]) {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads) {
            if (thread.id() == arguments.values[4]) {
                holder = thread;
                break;
            }
        }
    }
    struct Done {
        Thread& self;
        bool cancellable;
        bool boosting;
        ~Done()
        {
            if (boosting)
                self.set_boost_target(nullptr);
            if (cancellable)
                self.set_in_cancellation_point(false);
        }
    } done { self, cancellable, holder && holder.ptr() != &self };
    if (done.boosting)
        self.set_boost_target(holder.ptr());

    bool timed = arguments.values[3] != 0;
    u64 deadline = 0;
    if (timed) {
        auto timeout = TRY(copy_from_user<vulture_timespec>(arguments.values[3]));
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1'000'000'000)
            return Error::from_errno(EINVAL);
        deadline = Time::now() + Time::ticks_from_nanoseconds(static_cast<u64>(timeout.tv_sec) * 1'000'000'000 + static_cast<u64>(timeout.tv_nsec));
    }

    FutexWaiter waiter;
    waiter.key = key;
    auto& bucket = bucket_for(key);
    {
        SpinLockGuard guard(bucket.lock);
        bucket.waiters.append(waiter);
    }
    auto leave = [&] {
        SpinLockGuard guard(bucket.lock);
        if (waiter.node.is_in_list())
            bucket.waiters.remove(waiter);
    };

    auto word = copy_from_user<u32>(address);
    if (word.is_error()) {
        leave();
        return word.release_error();
    }
    if (word.value() != value) {
        leave();
        return Error::from_errno(EAGAIN);
    }

    auto woken = [&] { return waiter.woken.load(); };
    int error = 0;
    if (timed) {
        auto result = waiter.queue.wait_until_interruptible(woken, deadline);
        if (result == WaitQueue::WaitResult::Interrupted)
            error = EINTR;
        else if (result == WaitQueue::WaitResult::TimedOut)
            error = ETIMEDOUT;
    } else if (!waiter.queue.wait_until_interruptible(woken)) {
        error = EINTR;
    }
    leave();
    // Woken just as it gave up: the wake counted, so it must say so.
    if (error && !waiter.woken.load()) {
        if (cancellable && self.cancellation_is_due())
            return Error::from_errno(ECANCELED);
        return Error::from_errno(error);
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_thread_create(TrapFrame&, SyscallArguments const& arguments)
{
    auto parameters = TRY(copy_from_user<vulture_thread_parameters>(arguments.values[0]));
    auto entry = reinterpret_cast<FlatPtr>(parameters.entry);
    auto stack_top = reinterpret_cast<FlatPtr>(parameters.stack_top);
    auto tid_address = reinterpret_cast<FlatPtr>(parameters.tid_address);
    if (!is_user_range(entry, 4) || stack_top % 16 != 0 || stack_top == 0 || !is_user_range(stack_top - 1, 1) || tid_address % sizeof(u32) != 0)
        return Error::from_errno(EINVAL);
    if (tid_address) {
        u32 zero = 0;
        TRY(copy_to_user(tid_address, zero));
    }
    auto& creator = Thread::current();
    auto created = Thread::create(name(), [](void*) {
        auto& self = Thread::current();
        // Its creator may have been the last thing standing in the way of
        // the process ending.
        auto* process = self.process();
        if (process->must_end(self))
            process->exit_current_thread();
        Arch::return_to_user(self.user_trap_frame());
    },
        nullptr, { .qos = creator.qos(), .pinned_processor = {} });
    if (created.is_error())
        return Error::from_errno(EAGAIN);
    auto thread = created.release_value();
    Arch::prepare_user_entry(thread->user_trap_frame(), entry, stack_top);
    Arch::prepare_user_function_entry(thread->user_trap_frame());
    Arch::set_user_entry_argument(thread->user_trap_frame(), reinterpret_cast<FlatPtr>(parameters.argument));
    // (On x86_64, an address that doesn't exist would fault in the kernel, when it is loaded.)
    if (!is_user_range(reinterpret_cast<FlatPtr>(parameters.thread_pointer), 1))
        return Error::from_errno(EINVAL);
    Arch::set_user_thread_pointer(thread->m_context, reinterpret_cast<FlatPtr>(parameters.thread_pointer));
    thread->m_signal_mask.store(creator.m_signal_mask.load());
    thread->m_clear_tid_address = tid_address;
    u64 id = thread->id();
    if (tid_address) {
        u32 tid = static_cast<u32>(id);
        (void)copy_to_user(tid_address, tid);
    }
    thread->set_process(*this);
    thread->start();
    return static_cast<FlatPtr>(id);
}

// thread_exit(unmap_address, unmap_size): ends the calling thread, unmapping
// that memory first (a detached thread's stack, which it could not free while
// on it). The last thread ending ends the process, as exit(0) would.
ErrorOr<FlatPtr> Process::sys_thread_exit(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr unmap_address = arguments.values[0];
    size_t unmap_size = arguments.values[1];
    auto& thread = Thread::current();
    if (unmap_size)
        (void)m_address_space->unmap(unmap_address, unmap_size);
    if (thread.m_clear_tid_address) {
        u32 zero = 0;
        (void)copy_to_user(thread.m_clear_tid_address, zero);
        futex_wake(key_for(*m_address_space, thread.m_clear_tid_address), NumericLimits<u32>::max());
    }
    bool last = false;
    {
        // One step, or two threads ending at once could each leave the
        // other to be the last.
        SpinLockGuard guard(m_threads_lock);
        if (m_threads.size() == 1)
            last = true;
        else
            m_threads.remove(thread);
    }
    if (last)
        terminate(VULTURE_W_EXITED(0));
    exit_current_thread();
}

ErrorOr<FlatPtr> Process::sys_thread_kill(TrapFrame&, SyscallArguments const& arguments)
{
    u64 id = arguments.values[0];
    int signal = static_cast<int>(arguments.values[1]);
    if (signal < 0 || signal >= NSIG)
        return Error::from_errno(EINVAL);
    RefPtr<Thread> target;
    {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads) {
            if (thread.id() == id) {
                target = thread;
                break;
            }
        }
    }
    if (!target)
        return Error::from_errno(ESRCH);
    if (signal == 0)
        return 0;
    vulture_siginfo info {};
    info.si_code = SI_USER;
    info.si_pid = m_pid;
    send_signal_to_thread(*target, signal, info);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_set_thread_pointer(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr const pointer = arguments.values[0];
    if (!is_user_range(pointer, 1))
        return Error::from_errno(EINVAL);
    Arch::set_current_user_thread_pointer(pointer);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_set_thread_qos(TrapFrame&, SyscallArguments const& arguments)
{
    auto const wanted = arguments.values[0];
    auto& self = Thread::current();
    auto const previous = self.base_qos();
    if (wanted == VULTURE_QOS_JUST_ASKING)
        return static_cast<FlatPtr>(to_underlying(previous));
    if (wanted >= qos_class_count)
        return Error::from_errno(EINVAL);
    self.set_base_qos(static_cast<QoS>(wanted));
    return static_cast<FlatPtr>(to_underlying(previous));
}

ErrorOr<FlatPtr> Process::sys_thread_cancel_control(TrapFrame&, SyscallArguments const& arguments)
{
    auto const value = arguments.values[1];
    auto& self = Thread::current();
    switch (static_cast<i32>(arguments.values[0])) {
    case VULTURE_CANCEL_THREAD: {
        RefPtr<Thread> target;
        {
            SpinLockGuard guard(m_threads_lock);
            for (auto& thread : m_threads) {
                if (thread.id() == value) {
                    target = thread;
                    break;
                }
            }
        }
        if (!target)
            return Error::from_errno(ESRCH);
        target->request_cancel();
        target->interrupt_for_signal();
        return 0;
    }
    case VULTURE_CANCEL_ENABLE:
        self.set_cancel_enabled(value != 0);
        return 0;
    case VULTURE_CANCEL_ASYNC:
        self.set_cancel_async(value != 0);
        return 0;
    case VULTURE_CANCEL_HANDLER:
        m_cancel_handler.store(value);
        return 0;
    default:
        return Error::from_errno(EINVAL);
    }
}

}
