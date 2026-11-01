/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Lock.h"
#include <Kernel/API/POSIX/memory.h>
#include <LibBase/Types.h>
#include <LibSystem/ThreadPointer.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// POSIX threads, on the kernel's threads and futexes. A new thread's memory
// is one mapping: a guard page, the stack, the thread's TLS block (what the
// thread pointer points at) and this library's record of it, so all of it
// goes at once, by whoever joins it or, if it is detached, by the kernel as
// the thread ends.

using LibC::futex_wait;
using LibC::futex_wake;
using LibC::lock_word;
using LibC::lock_word_contended;
using LibC::Lock;
using LibC::LockGuard;
using LibC::try_lock_word;
using LibC::unlock_word;
using System::Syscall;

namespace {

constexpr size_t page_size = 4096;
// macOS's for threads other than the main one.
constexpr size_t default_stack_size = 512 * 1024;
// The main thread's stack, as the kernel sets it up.
constexpr FlatPtr main_stack_top = 0x0000'8000'0000'0000;
constexpr size_t main_stack_size = 8 * 1024 * 1024;

enum ThreadState : unsigned {
    Joinable,
    Detached,
    Exiting,
};

struct ExitHandler {
    void (*function)(void*);
    void* argument;
    ExitHandler* next;
};

size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void pause_processor()
{
    asm volatile("yield");
}

}

struct vulture_pthread {
    // Written by the kernel before the thread starts; zero once it has ended.
    unsigned tid;
    unsigned state; // ThreadState.
    unsigned joined;
    void* (*start_routine)(void*);
    void* argument;
    void* result;
    // What to unmap when done with the thread; null for the main thread.
    void* mapping;
    size_t mapping_size;
    void* stack_top;
    size_t stack_size;
    __pthread_handler_rec* cleanup_handlers;
    ExitHandler* exit_handlers;
    char name[64];
    struct {
        unsigned generation;
        void* value;
    } specific[PTHREAD_KEYS_MAX];
};

namespace {

vulture_pthread s_main_thread;
// The thread main() ran on, or the one that forked.
pthread_t s_initial_thread = &s_main_thread;
unsigned s_thread_count = 1;

pthread_t self()
{
    return static_cast<pthread_t>(System::thread_control_block()->thread);
}

// --- Thread-specific data ---

struct Key {
    bool in_use;
    // Bumped by each create and delete, so a key used again reads as null.
    unsigned generation;
    void (*destructor)(void*);
};

Key s_keys[PTHREAD_KEYS_MAX];
Lock s_keys_lock;

void run_key_destructors(pthread_t thread)
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
        bool any = false;
        for (unsigned key = 0; key < PTHREAD_KEYS_MAX; ++key) {
            auto& slot = thread->specific[key];
            if (!slot.value)
                continue;
            void (*destructor)(void*) = nullptr;
            bool current;
            {
                LockGuard guard(s_keys_lock);
                current = s_keys[key].in_use && s_keys[key].generation == slot.generation;
                destructor = s_keys[key].destructor;
            }
            void* value = slot.value;
            slot.value = nullptr;
            if (current && destructor) {
                destructor(value);
                any = true;
            }
        }
        if (!any)
            return;
    }
}

// --- fork ---

struct ForkHandlers {
    void (*prepare)();
    void (*parent)();
    void (*child)();
};

constexpr size_t max_fork_handlers = 32;
ForkHandlers s_fork_handlers[max_fork_handlers];
size_t s_fork_handler_count = 0;
Lock s_fork_handlers_lock;

// An absolute time (on `clock`) as what is left of it from now; false if
// it is past.
bool time_left(clockid_t clock, timespec const* absolute, timespec& left)
{
    timespec now {};
    clock_gettime(clock, &now);
    left.tv_sec = absolute->tv_sec - now.tv_sec;
    left.tv_nsec = absolute->tv_nsec - now.tv_nsec;
    if (left.tv_nsec < 0) {
        left.tv_nsec += 1'000'000'000;
        --left.tv_sec;
    }
    return left.tv_sec >= 0;
}

bool is_valid_time(timespec const* time)
{
    return time->tv_nsec >= 0 && time->tv_nsec < 1'000'000'000;
}

}

extern "C" {

// Taken around fork() by the modules with locks of their own.
void __malloc_fork_lock();
void __malloc_fork_unlock();
void __stdio_fork_lock();
void __stdio_fork_unlock(int child);
void __exit_fork_lock();
void __exit_fork_unlock(int child);

// The C library's setup: the first thread's record, in its control block.
void __pthread_initialize()
{
    s_main_thread.tid = static_cast<unsigned>(System::syscall(Syscall::gettid));
    s_main_thread.state = Joinable;
    s_main_thread.stack_top = reinterpret_cast<void*>(main_stack_top);
    s_main_thread.stack_size = main_stack_size;
    System::thread_control_block()->thread = &s_main_thread;
}

int __libc_register_thread_exit_handler(void (*function)(void*), void* argument)
{
    auto* handler = static_cast<ExitHandler*>(malloc(sizeof(ExitHandler)));
    if (!handler)
        return -1;
    auto* thread = self();
    *handler = { function, argument, thread->exit_handlers };
    thread->exit_handlers = handler;
    return 0;
}

// thread_local objects' destructors, most recently constructed first.
void __libc_run_thread_exit_handlers()
{
    auto* thread = self();
    while (auto* handler = thread->exit_handlers) {
        thread->exit_handlers = handler->next;
        auto function = handler->function;
        auto* argument = handler->argument;
        free(handler);
        function(argument);
    }
}

pid_t fork()
{
    ForkHandlers handlers[max_fork_handlers];
    size_t count;
    {
        LockGuard guard(s_fork_handlers_lock);
        count = s_fork_handler_count;
        for (size_t i = 0; i < count; ++i)
            handlers[i] = s_fork_handlers[i];
    }
    for (size_t i = count; i > 0; --i) {
        if (handlers[i - 1].prepare)
            handlers[i - 1].prepare();
    }
    // Nobody may be halfway through these as the memory is copied: in the
    // child, the threads that would finish are gone.
    s_fork_handlers_lock.lock();
    s_keys_lock.lock();
    __exit_fork_lock();
    __stdio_fork_lock();
    __malloc_fork_lock();
    long result = System::syscall(Syscall::fork);
    bool child = result == 0;
    if (child) {
        auto* thread = self();
        thread->tid = static_cast<unsigned>(System::syscall(Syscall::gettid));
        s_initial_thread = thread;
        s_thread_count = 1;
    }
    __malloc_fork_unlock();
    __stdio_fork_unlock(child);
    __exit_fork_unlock(child);
    s_keys_lock.unlock();
    s_fork_handlers_lock.unlock();
    for (size_t i = 0; i < count; ++i) {
        auto function = child ? handlers[i].child : handlers[i].parent;
        if (function)
            function();
    }
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return static_cast<pid_t>(result);
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    LockGuard guard(s_fork_handlers_lock);
    if (s_fork_handler_count == max_fork_handlers)
        return ENOMEM;
    s_fork_handlers[s_fork_handler_count++] = { prepare, parent, child };
    return 0;
}

// --- Threads ---

// Where every thread starts (the kernel passes the record as the argument).
[[noreturn]] static void thread_entry(void* argument)
{
    auto* thread = static_cast<pthread_t>(argument);
    pthread_exit(thread->start_routine(thread->argument));
}

int pthread_create(pthread_t* result, pthread_attr_t const* attributes, void* (*start_routine)(void*), void* argument)
{
    size_t stack_size = attributes && attributes->stack_size ? attributes->stack_size : default_stack_size;
    size_t guard_size = attributes ? attributes->guard_size : page_size;
    void* given_stack = attributes ? attributes->stack_address : nullptr;
    auto const* image = System::thread_control_block()->image;
    size_t alignment = image->alignment < 16 ? 16 : image->alignment;

    // Low to high: guard, stack, TLS block, record.
    size_t below = given_stack ? 0 : align_up(guard_size, page_size) + align_up(stack_size, page_size);
    size_t total = align_up(below + alignment + image->size + 16 + sizeof(vulture_pthread), page_size);
    long mapped = System::syscall(Syscall::mmap, 0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long>(-1), 0);
    if (mapped < 0)
        return EAGAIN;
    auto* mapping = reinterpret_cast<u8*>(mapped);
    if (!given_stack && guard_size)
        System::syscall(Syscall::mprotect, static_cast<unsigned long>(mapped), align_up(guard_size, page_size), PROT_NONE);

    auto block = align_up(reinterpret_cast<FlatPtr>(mapping + below), alignment);
    memcpy(reinterpret_cast<void*>(block), image->data, image->size);
    auto* control = reinterpret_cast<System::ThreadControlBlock*>(block);
    auto* thread = reinterpret_cast<pthread_t>(align_up(block + image->size, 16));
    control->image = image;
    control->thread = thread;

    auto stack_top = given_stack ? (reinterpret_cast<FlatPtr>(given_stack) + stack_size) & ~static_cast<FlatPtr>(15) : reinterpret_cast<FlatPtr>(mapping + below);
    thread->state = attributes && attributes->detach_state == PTHREAD_CREATE_DETACHED ? Detached : Joinable;
    thread->start_routine = start_routine;
    thread->argument = argument;
    thread->mapping = mapping;
    thread->mapping_size = total;
    thread->stack_top = reinterpret_cast<void*>(stack_top);
    thread->stack_size = stack_size;

    __atomic_add_fetch(&s_thread_count, 1, __ATOMIC_SEQ_CST);
    vulture_thread_parameters parameters {
        reinterpret_cast<void*>(thread_entry),
        thread,
        reinterpret_cast<void*>(stack_top),
        control,
        &thread->tid,
    };
    // The new thread may be gone (if detached) by the time this returns:
    // nothing below looks at it.
    long created = System::syscall(Syscall::thread_create, reinterpret_cast<unsigned long>(&parameters));
    if (created < 0) {
        __atomic_sub_fetch(&s_thread_count, 1, __ATOMIC_SEQ_CST);
        System::syscall(Syscall::munmap, static_cast<unsigned long>(mapped), total);
        return static_cast<int>(-created) == EAGAIN ? EAGAIN : static_cast<int>(-created);
    }
    *result = thread;
    return 0;
}

void pthread_exit(void* result)
{
    auto* thread = self();
    while (auto* handler = thread->cleanup_handlers) {
        thread->cleanup_handlers = handler->next;
        handler->routine(handler->argument);
    }
    __libc_run_thread_exit_handlers();
    run_key_destructors(thread);
    // The last thread: as if the program had called exit(0).
    if (__atomic_sub_fetch(&s_thread_count, 1, __ATOMIC_SEQ_CST) == 0)
        exit(0);

    thread->result = result;
    unsigned previous = __atomic_exchange_n(&thread->state, static_cast<unsigned>(Exiting), __ATOMIC_ACQ_REL);
    unsigned long unmap_address = 0, unmap_size = 0;
    if (previous == Detached && thread->mapping) {
        unmap_address = reinterpret_cast<unsigned long>(thread->mapping);
        unmap_size = thread->mapping_size;
    }
    // The kernel clears the word of threads it started; not the first's.
    if (!thread->mapping) {
        __atomic_store_n(&thread->tid, 0u, __ATOMIC_RELEASE);
        futex_wake(&thread->tid, 0x7fffffff);
    }
    System::syscall(Syscall::thread_exit, unmap_address, unmap_size);
    __builtin_trap();
}

static void wait_until_ended(pthread_t thread)
{
    for (;;) {
        unsigned tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
        if (tid == 0)
            return;
        futex_wait(&thread->tid, tid);
    }
}

int pthread_join(pthread_t thread, void** result)
{
    if (thread == self())
        return EDEADLK;
    if (__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == Detached)
        return EINVAL;
    if (__atomic_exchange_n(&thread->joined, 1u, __ATOMIC_ACQ_REL))
        return EINVAL;
    wait_until_ended(thread);
    if (result)
        *result = thread->result;
    if (thread->mapping)
        System::syscall(Syscall::munmap, reinterpret_cast<unsigned long>(thread->mapping), thread->mapping_size);
    return 0;
}

int pthread_detach(pthread_t thread)
{
    unsigned previous = __atomic_exchange_n(&thread->state, static_cast<unsigned>(Detached), __ATOMIC_ACQ_REL);
    if (previous == Detached)
        return EINVAL;
    // Already on its way out, not knowing it would have to clean up.
    if (previous == Exiting) {
        wait_until_ended(thread);
        if (thread->mapping)
            System::syscall(Syscall::munmap, reinterpret_cast<unsigned long>(thread->mapping), thread->mapping_size);
    }
    return 0;
}

pthread_t pthread_self(void)
{
    return self();
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_kill(pthread_t thread, int signal)
{
    long result = System::syscall(Syscall::thread_kill, __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE), static_cast<unsigned long>(signal));
    return result < 0 ? static_cast<int>(-result) : 0;
}

int pthread_sigmask(int how, sigset_t const* set, sigset_t* old_set)
{
    long result = System::syscall(Syscall::sigprocmask, static_cast<unsigned long>(how), reinterpret_cast<unsigned long>(set), reinterpret_cast<unsigned long>(old_set));
    return result < 0 ? static_cast<int>(-result) : 0;
}

int pthread_threadid_np(pthread_t thread, __UINT64_TYPE__* id)
{
    if (!id)
        return EINVAL;
    *id = thread ? thread->tid : self()->tid;
    return 0;
}

int pthread_setname_np(char const* name)
{
    if (strlen(name) >= sizeof(self()->name))
        return ENAMETOOLONG;
    strcpy(self()->name, name);
    return 0;
}

int pthread_getname_np(pthread_t thread, char* name, size_t size)
{
    if (strlen(thread->name) >= size)
        return ERANGE;
    strcpy(name, thread->name);
    return 0;
}

int pthread_main_np(void)
{
    return self() == s_initial_thread;
}

void* pthread_get_stackaddr_np(pthread_t thread)
{
    return thread->stack_top;
}

size_t pthread_get_stacksize_np(pthread_t thread)
{
    return thread->stack_size;
}

void pthread_yield_np(void)
{
    sched_yield();
}

int pthread_once(pthread_once_t* once, void (*routine)(void))
{
    // 0: not yet; 1: running; 2: running, with others waiting; 3: done.
    for (;;) {
        unsigned state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (state == 3)
            return 0;
        if (state == 0) {
            unsigned expected = 0;
            if (!__atomic_compare_exchange_n(&once->state, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                continue;
            routine();
            if (__atomic_exchange_n(&once->state, 3u, __ATOMIC_RELEASE) == 2)
                futex_wake(&once->state, 0x7fffffff);
            return 0;
        }
        unsigned expected = 1;
        if (state == 2 || __atomic_compare_exchange_n(&once->state, &expected, 2u, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            futex_wait(&once->state, 2);
    }
}

// --- Thread attributes ---

int pthread_attr_init(pthread_attr_t* attributes)
{
    *attributes = { PTHREAD_CREATE_JOINABLE, 0, default_stack_size, page_size, nullptr };
    return 0;
}

int pthread_attr_destroy(pthread_attr_t*)
{
    return 0;
}

int pthread_attr_getdetachstate(pthread_attr_t const* attributes, int* state)
{
    *state = attributes->detach_state;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attributes, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attributes->detach_state = state;
    return 0;
}

int pthread_attr_getstacksize(pthread_attr_t const* attributes, size_t* size)
{
    *size = attributes->stack_size;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attributes, size_t size)
{
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    attributes->stack_size = size;
    return 0;
}

int pthread_attr_getguardsize(pthread_attr_t const* attributes, size_t* size)
{
    *size = attributes->guard_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t* attributes, size_t size)
{
    attributes->guard_size = size;
    return 0;
}

int pthread_attr_getstack(pthread_attr_t const* attributes, void** address, size_t* size)
{
    *address = attributes->stack_address;
    *size = attributes->stack_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t* attributes, void* address, size_t size)
{
    if (size < PTHREAD_STACK_MIN)
        return EINVAL;
    attributes->stack_address = address;
    attributes->stack_size = size;
    return 0;
}

// --- Mutexes ---

int pthread_mutex_init(pthread_mutex_t* mutex, pthread_mutexattr_t const* attributes)
{
    *mutex = { 0, attributes ? attributes->type : PTHREAD_MUTEX_NORMAL, nullptr, 0, 0 };
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    return __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) ? EBUSY : 0;
}

// For the kinds that keep an owner: 0 to carry on and take the word, else
// the result. (The owner field can only match the caller if the caller put
// it there, so reading it unlocked is fine.)
static int check_owner_before_lock(pthread_mutex_t* mutex, pthread_t thread, bool& done)
{
    done = false;
    if (mutex->type == PTHREAD_MUTEX_NORMAL || __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != thread)
        return 0;
    done = true;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        ++mutex->count;
        return 0;
    }
    return EDEADLK;
}

int pthread_mutex_lock(pthread_mutex_t* mutex)
{
    auto* thread = mutex->type == PTHREAD_MUTEX_NORMAL ? nullptr : self();
    bool done;
    int result = check_owner_before_lock(mutex, thread, done);
    if (done)
        return result;
    lock_word(&mutex->state);
    if (thread) {
        __atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
        mutex->count = 1;
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex)
{
    auto* thread = mutex->type == PTHREAD_MUTEX_NORMAL ? nullptr : self();
    if (thread && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == thread) {
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
            ++mutex->count;
            return 0;
        }
        return EBUSY;
    }
    if (!try_lock_word(&mutex->state))
        return EBUSY;
    if (thread) {
        __atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
        mutex->count = 1;
    }
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* mutex, timespec const* absolute_time)
{
    auto* thread = mutex->type == PTHREAD_MUTEX_NORMAL ? nullptr : self();
    bool done;
    int result = check_owner_before_lock(mutex, thread, done);
    if (done)
        return result;
    if (!try_lock_word(&mutex->state)) {
        if (!is_valid_time(absolute_time))
            return EINVAL;
        while (__atomic_exchange_n(&mutex->state, 2u, __ATOMIC_ACQUIRE) != 0) {
            timespec left;
            if (!time_left(CLOCK_REALTIME, absolute_time, left))
                return ETIMEDOUT;
            if (futex_wait(&mutex->state, 2, &left) == -ETIMEDOUT)
                return ETIMEDOUT;
        }
    }
    if (thread) {
        __atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
        mutex->count = 1;
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    if (mutex->type != PTHREAD_MUTEX_NORMAL) {
        if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != self())
            return EPERM;
        if (--mutex->count > 0)
            return 0;
        __atomic_store_n(&mutex->owner, static_cast<void*>(nullptr), __ATOMIC_RELAXED);
    }
    unlock_word(&mutex->state);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* attributes)
{
    attributes->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t*)
{
    return 0;
}

int pthread_mutexattr_gettype(pthread_mutexattr_t const* attributes, int* type)
{
    *type = attributes->type;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attributes, int type)
{
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_ERRORCHECK && type != PTHREAD_MUTEX_RECURSIVE)
        return EINVAL;
    attributes->type = type;
    return 0;
}

int pthread_mutexattr_getpshared(pthread_mutexattr_t const*, int* shared)
{
    *shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t*, int shared)
{
    // Futexes are private to a process.
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

// --- Condition variables ---

int pthread_cond_init(pthread_cond_t* condition, pthread_condattr_t const* attributes)
{
    *condition = { 0, 0, attributes ? attributes->clock : CLOCK_REALTIME, 0 };
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* condition)
{
    return __atomic_load_n(&condition->waiters, __ATOMIC_RELAXED) ? EBUSY : 0;
}

static int cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex, timespec const* absolute_time)
{
    if (mutex->type != PTHREAD_MUTEX_NORMAL && __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != self())
        return EPERM;
    if (absolute_time && !is_valid_time(absolute_time))
        return EINVAL;
    // Read before the mutex goes: a signal after that changes the sequence,
    // and the wait below then does not sleep.
    unsigned sequence = __atomic_load_n(&condition->sequence, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&condition->waiters, 1, __ATOMIC_SEQ_CST);
    void* owner = mutex->owner;
    unsigned count = mutex->count;
    if (mutex->type != PTHREAD_MUTEX_NORMAL) {
        __atomic_store_n(&mutex->owner, static_cast<void*>(nullptr), __ATOMIC_RELAXED);
        mutex->count = 0;
    }
    unlock_word(&mutex->state);

    int result = 0;
    timespec left;
    if (absolute_time && !time_left(condition->clock, absolute_time, left)) {
        result = ETIMEDOUT;
    } else if (futex_wait(&condition->sequence, sequence, absolute_time ? &left : nullptr) == -ETIMEDOUT) {
        result = ETIMEDOUT;
    }

    __atomic_sub_fetch(&condition->waiters, 1, __ATOMIC_SEQ_CST);
    lock_word_contended(&mutex->state);
    if (mutex->type != PTHREAD_MUTEX_NORMAL) {
        __atomic_store_n(&mutex->owner, owner, __ATOMIC_RELAXED);
        mutex->count = count;
    }
    return result;
}

int pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex)
{
    return cond_wait(condition, mutex, nullptr);
}

int pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex, timespec const* absolute_time)
{
    return cond_wait(condition, mutex, absolute_time);
}

int pthread_cond_signal(pthread_cond_t* condition)
{
    if (__atomic_load_n(&condition->waiters, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_SEQ_CST);
        futex_wake(&condition->sequence, 1);
    }
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* condition)
{
    if (__atomic_load_n(&condition->waiters, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_SEQ_CST);
        futex_wake(&condition->sequence, 0x7fffffff);
    }
    return 0;
}

int pthread_condattr_init(pthread_condattr_t* attributes)
{
    attributes->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t*)
{
    return 0;
}

int pthread_condattr_getclock(pthread_condattr_t const* attributes, clockid_t* clock)
{
    *clock = attributes->clock;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t* attributes, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    attributes->clock = clock;
    return 0;
}

// --- Read-write locks ---

static constexpr unsigned rwlock_writing = 0xffffffff;

int pthread_rwlock_init(pthread_rwlock_t* lock, pthread_rwlockattr_t const*)
{
    *lock = { 0, 0, nullptr };
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* lock)
{
    return __atomic_load_n(&lock->state, __ATOMIC_RELAXED) ? EBUSY : 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* lock)
{
    for (;;) {
        unsigned state = __atomic_load_n(&lock->state, __ATOMIC_SEQ_CST);
        if (state == rwlock_writing)
            return EBUSY;
        if (state == rwlock_writing - 1)
            return EAGAIN;
        if (__atomic_compare_exchange_n(&lock->state, &state, state + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t* lock)
{
    for (;;) {
        int result = pthread_rwlock_tryrdlock(lock);
        if (result != EBUSY)
            return result;
        if (__atomic_load_n(&lock->writer, __ATOMIC_RELAXED) == self())
            return EDEADLK;
        __atomic_add_fetch(&lock->waiters, 1, __ATOMIC_SEQ_CST);
        futex_wait(&lock->state, rwlock_writing);
        __atomic_sub_fetch(&lock->waiters, 1, __ATOMIC_SEQ_CST);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* lock)
{
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&lock->state, &expected, rwlock_writing, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return EBUSY;
    __atomic_store_n(&lock->writer, self(), __ATOMIC_RELAXED);
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t* lock)
{
    for (;;) {
        if (pthread_rwlock_trywrlock(lock) == 0)
            return 0;
        if (__atomic_load_n(&lock->writer, __ATOMIC_RELAXED) == self())
            return EDEADLK;
        unsigned state = __atomic_load_n(&lock->state, __ATOMIC_SEQ_CST);
        if (state == 0)
            continue;
        __atomic_add_fetch(&lock->waiters, 1, __ATOMIC_SEQ_CST);
        futex_wait(&lock->state, state);
        __atomic_sub_fetch(&lock->waiters, 1, __ATOMIC_SEQ_CST);
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t* lock)
{
    unsigned state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    bool free;
    if (state == rwlock_writing) {
        if (__atomic_load_n(&lock->writer, __ATOMIC_RELAXED) != self())
            return EPERM;
        __atomic_store_n(&lock->writer, static_cast<void*>(nullptr), __ATOMIC_RELAXED);
        __atomic_store_n(&lock->state, 0u, __ATOMIC_SEQ_CST);
        free = true;
    } else {
        if (state == 0)
            return EPERM;
        free = __atomic_sub_fetch(&lock->state, 1, __ATOMIC_SEQ_CST) == 0;
    }
    if (free && __atomic_load_n(&lock->waiters, __ATOMIC_SEQ_CST))
        futex_wake(&lock->state, 0x7fffffff);
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t*)
{
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t*)
{
    return 0;
}

// --- Barriers and spin locks ---

int pthread_barrier_init(pthread_barrier_t* barrier, pthread_barrierattr_t const*, unsigned count)
{
    if (count == 0)
        return EINVAL;
    *barrier = { count, 0, 0, 0 };
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t* barrier)
{
    return __atomic_load_n(&barrier->waiting, __ATOMIC_RELAXED) ? EBUSY : 0;
}

int pthread_barrier_wait(pthread_barrier_t* barrier)
{
    lock_word(&barrier->lock);
    unsigned generation = barrier->generation;
    if (++barrier->waiting == barrier->count) {
        barrier->waiting = 0;
        __atomic_store_n(&barrier->generation, generation + 1, __ATOMIC_RELEASE);
        unlock_word(&barrier->lock);
        futex_wake(&barrier->generation, 0x7fffffff);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    unlock_word(&barrier->lock);
    while (__atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE) == generation)
        futex_wait(&barrier->generation, generation);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t*)
{
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t*)
{
    return 0;
}

int pthread_spin_init(pthread_spinlock_t* lock, int)
{
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t*)
{
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t* lock)
{
    int expected = 0;
    return __atomic_compare_exchange_n(lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t* lock)
{
    while (pthread_spin_trylock(lock) != 0) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            pause_processor();
    }
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    return 0;
}

// --- Thread-specific data ---

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*))
{
    LockGuard guard(s_keys_lock);
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; ++i) {
        if (!s_keys[i].in_use) {
            s_keys[i].in_use = true;
            ++s_keys[i].generation;
            s_keys[i].destructor = destructor;
            *key = i;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX)
        return EINVAL;
    LockGuard guard(s_keys_lock);
    if (!s_keys[key].in_use)
        return EINVAL;
    s_keys[key].in_use = false;
    ++s_keys[key].generation;
    return 0;
}

void* pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX)
        return nullptr;
    auto const& slot = self()->specific[key];
    // Set under a generation since gone: this key was deleted since.
    return slot.generation == __atomic_load_n(&s_keys[key].generation, __ATOMIC_RELAXED) ? slot.value : nullptr;
}

int pthread_setspecific(pthread_key_t key, void const* value)
{
    if (key >= PTHREAD_KEYS_MAX)
        return EINVAL;
    unsigned generation = __atomic_load_n(&s_keys[key].generation, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&s_keys[key].in_use, __ATOMIC_RELAXED))
        return EINVAL;
    auto& slot = self()->specific[key];
    slot.generation = generation;
    slot.value = const_cast<void*>(value);
    return 0;
}

// --- Cleanup handlers ---

void __pthread_cleanup_push(__pthread_handler_rec* handler)
{
    auto* thread = self();
    handler->next = thread->cleanup_handlers;
    thread->cleanup_handlers = handler;
}

void __pthread_cleanup_pop(int execute)
{
    auto* thread = self();
    auto* handler = thread->cleanup_handlers;
    thread->cleanup_handlers = handler->next;
    if (execute)
        handler->routine(handler->argument);
}

// --- Semaphores ---

int sem_init(sem_t* semaphore, int shared, unsigned value)
{
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (shared) {
        errno = ENOSYS;
        return -1;
    }
    *semaphore = { value, 0 };
    return 0;
}

int sem_destroy(sem_t*)
{
    return 0;
}

int sem_post(sem_t* semaphore)
{
    unsigned value = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
    do {
        if (value == SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(&semaphore->value, &value, value + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    if (__atomic_load_n(&semaphore->waiters, __ATOMIC_SEQ_CST))
        futex_wake(&semaphore->value, 1);
    return 0;
}

int sem_trywait(sem_t* semaphore)
{
    unsigned value = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
    while (value > 0) {
        if (__atomic_compare_exchange_n(&semaphore->value, &value, value - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

static int sem_wait_until(sem_t* semaphore, timespec const* absolute_time)
{
    int saved = errno;
    for (;;) {
        if (sem_trywait(semaphore) == 0) {
            errno = saved;
            return 0;
        }
        timespec left;
        if (absolute_time && !time_left(CLOCK_REALTIME, absolute_time, left)) {
            errno = ETIMEDOUT;
            return -1;
        }
        __atomic_add_fetch(&semaphore->waiters, 1, __ATOMIC_SEQ_CST);
        long result = futex_wait(&semaphore->value, 0, absolute_time ? &left : nullptr);
        __atomic_sub_fetch(&semaphore->waiters, 1, __ATOMIC_SEQ_CST);
        if (result == -EINTR) {
            errno = EINTR;
            return -1;
        }
    }
}

int sem_wait(sem_t* semaphore)
{
    return sem_wait_until(semaphore, nullptr);
}

int sem_timedwait(sem_t* semaphore, timespec const* absolute_time)
{
    if (!is_valid_time(absolute_time)) {
        errno = EINVAL;
        return -1;
    }
    return sem_wait_until(semaphore, absolute_time);
}

int sem_getvalue(sem_t* semaphore, int* value)
{
    *value = static_cast<int>(__atomic_load_n(&semaphore->value, __ATOMIC_RELAXED));
    return 0;
}
}

namespace LibC {

void lock_word_slowly(unsigned* word)
{
    for (int spin = 0; spin < 128; ++spin) {
        if (__atomic_load_n(word, __ATOMIC_RELAXED) == 0 && try_lock_word(word))
            return;
        pause_processor();
    }
    lock_word_contended(word);
}

}
