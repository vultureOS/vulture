/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <Kernel/API/POSIX/thread.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Listener.h>
#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Threads: pthreads and everything under them. Thread-local storage (ELF TLS
// in the program and in libSystem), errno, the futex, per-thread signal masks
// and pending signals, sigaltstack, and what happens to a process with several
// threads when one exits, execs, forks, faults or is stopped. Scenarios that
// end the process run in a child.

namespace {

void pause_briefly()
{
#if defined(__x86_64__)
    asm volatile("pause");
#else
    asm volatile("yield");
#endif
}

using System::Syscall;

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

long now_microseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1'000'000 + now.tv_nsec / 1000;
}

// The child's status, or -1 (having killed it) if it takes too long.
int wait_for(pid_t child, int seconds = 10)
{
    for (int waited = 0; waited < seconds * 200; ++waited) {
        int status = 0;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            return status;
        pause_milliseconds(5);
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return -1;
}

template<typename Body>
int in_child(Body body, int seconds = 10)
{
    pid_t child = fork();
    if (child == 0)
        _exit(body());
    return wait_for(child, seconds);
}

// The kernel's count of this process's threads.
unsigned long long thread_count()
{
    size_t length = 0;
    sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0);
    length += 32 * sizeof(vulture_process_info);
    auto* processes = static_cast<vulture_process_info*>(malloc(length));
    sysctlbyname("kern.proc.all", processes, &length, nullptr, 0);
    unsigned long long count = 0;
    for (size_t i = 0; i < length / sizeof(vulture_process_info); ++i) {
        if (processes[i].pid == getpid())
            count = processes[i].threads;
    }
    free(processes);
    return count;
}

// --- Thread-local storage ---

int s_global;
thread_local int t_initialized = 42;
thread_local int t_zeroed;
// A relocation in the initial data.
thread_local int* t_pointer = &s_global;
// More than the control block's alignment.
alignas(64) thread_local char t_aligned[64] = { 'a' };

int s_destroyed = 0;
struct Tracked {
    int value { 5 };
    ~Tracked() { __atomic_add_fetch(&s_destroyed, 1, __ATOMIC_SEQ_CST); }
};
thread_local Tracked t_tracked;

// errno itself, reached as another module's variable.
extern "C" thread_local int __libc_errno;

void* check_thread_locals(void* argument)
{
    auto* failures = static_cast<int*>(argument);
    if (t_initialized != 42 || t_zeroed != 0 || t_pointer != &s_global || t_aligned[0] != 'a' || reinterpret_cast<uintptr_t>(t_aligned) % 64 != 0)
        ++*failures;
    t_initialized = 7;
    t_zeroed = 8;
    t_aligned[0] = 'b';
    if (t_tracked.value != 5)
        ++*failures;
    t_tracked.value = 6;
    errno = EDOM;
    pause_milliseconds(5);
    if (errno != EDOM || &errno != &__libc_errno)
        ++*failures;
    return nullptr;
}

void test_thread_local_storage()
{
    errno = ERANGE;
    t_initialized = 100;
    int failures = 0;
    pthread_t threads[4];
    for (auto& thread : threads)
        CHECK(pthread_create(&thread, nullptr, check_thread_locals, &failures) == 0);
    for (auto& thread : threads)
        CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(failures == 0);
    CHECK(errno == ERANGE);
    CHECK(t_initialized == 100);
    CHECK(t_zeroed == 0 && t_aligned[0] == 'a');
    CHECK(__atomic_load_n(&s_destroyed, __ATOMIC_SEQ_CST) == 4);
}

// --- Creating and joining ---

void* add_one(void* argument)
{
    return reinterpret_cast<void*>(reinterpret_cast<long>(argument) + 1);
}

void* exit_with_argument(void* argument)
{
    pthread_exit(argument);
}

long s_detached_done = 0;
void* detached_body(void*)
{
    __atomic_add_fetch(&s_detached_done, 1, __ATOMIC_SEQ_CST);
    return nullptr;
}

void* big_stack_user(void*)
{
    volatile char big[1024 * 1024];
    for (size_t i = 0; i < sizeof(big); i += 4096)
        big[i] = 1;
    return reinterpret_cast<void*>(static_cast<long>(big[4096]));
}

void test_create_and_join()
{
    void* result = nullptr;
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, add_one, reinterpret_cast<void*>(41)) == 0);
    CHECK(pthread_join(thread, &result) == 0 && reinterpret_cast<long>(result) == 42);
    CHECK(pthread_create(&thread, nullptr, exit_with_argument, reinterpret_cast<void*>(9)) == 0);
    CHECK(pthread_join(thread, &result) == 0 && reinterpret_cast<long>(result) == 9);

    CHECK(pthread_equal(pthread_self(), pthread_self()));
    CHECK(pthread_join(pthread_self(), nullptr) == EDEADLK);
    CHECK(pthread_main_np() == 1);
    CHECK(thread_count() == 1);

    // The kernel counts them.
    static pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, nullptr, 4);
    pthread_t waiting[3];
    for (auto& one : waiting) {
        CHECK(pthread_create(&one, nullptr, [](void*) -> void* {
            pthread_barrier_wait(&barrier);
            return nullptr;
        },
                  nullptr)
            == 0);
    }
    CHECK(thread_count() == 4);
    pthread_barrier_wait(&barrier);
    for (auto& one : waiting)
        pthread_join(one, nullptr);
    pthread_barrier_destroy(&barrier);

    // Many at once, more than once: nothing leaks or runs out.
    for (int round = 0; round < 5; ++round) {
        pthread_t many[64];
        for (long i = 0; i < 64; ++i)
            CHECK(pthread_create(&many[i], nullptr, add_one, reinterpret_cast<void*>(i)) == 0);
        for (long i = 0; i < 64; ++i) {
            CHECK(pthread_join(many[i], &result) == 0);
            CHECK(reinterpret_cast<long>(result) == i + 1);
        }
    }

    // Detached: they clean up after themselves.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 200; ++i) {
        CHECK(pthread_create(&thread, &attributes, detached_body, nullptr) == 0);
        if (i % 16 == 0)
            pause_milliseconds(1);
    }
    for (int i = 0; i < 2000 && __atomic_load_n(&s_detached_done, __ATOMIC_SEQ_CST) != 200; ++i)
        pause_milliseconds(1);
    CHECK(__atomic_load_n(&s_detached_done, __ATOMIC_SEQ_CST) == 200);
    pthread_attr_destroy(&attributes);

    // Detaching one after the fact.
    CHECK(pthread_create(&thread, nullptr, add_one, nullptr) == 0);
    CHECK(pthread_detach(thread) == 0);

    // A stack bigger than the default, and one of the caller's.
    pthread_attr_init(&attributes);
    CHECK(pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024) == 0);
    CHECK(pthread_attr_setstacksize(&attributes, 100) == EINVAL);
    CHECK(pthread_create(&thread, &attributes, big_stack_user, nullptr) == 0);
    CHECK(pthread_join(thread, &result) == 0 && reinterpret_cast<long>(result) == 1);
    pthread_attr_destroy(&attributes);

    static char my_stack[64 * 1024] __attribute__((aligned(16)));
    pthread_attr_init(&attributes);
    CHECK(pthread_attr_setstack(&attributes, my_stack, sizeof(my_stack)) == 0);
    CHECK(pthread_create(&thread, &attributes, [](void* argument) -> void* {
        int local = 0;
        auto address = reinterpret_cast<char*>(&local);
        return reinterpret_cast<void*>(static_cast<long>(address >= static_cast<char*>(argument) && address < static_cast<char*>(argument) + sizeof(my_stack)));
    },
              my_stack)
        == 0);
    CHECK(pthread_join(thread, &result) == 0 && reinterpret_cast<long>(result) == 1);
    pthread_attr_destroy(&attributes);

    char name[64] = {};
    CHECK(pthread_setname_np("test main") == 0);
    CHECK(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && strcmp(name, "test main") == 0);
    __UINT64_TYPE__ id = 0;
    CHECK(pthread_threadid_np(nullptr, &id) == 0 && id == static_cast<__UINT64_TYPE__>(gettid()));
}

// --- Mutual exclusion ---

constexpr int contention_threads = 8;
constexpr int contention_rounds = 20000;

pthread_mutex_t s_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
long s_counter = 0;

void* count_up(void*)
{
    for (int i = 0; i < contention_rounds; ++i) {
        pthread_mutex_lock(&s_counter_mutex);
        ++s_counter;
        pthread_mutex_unlock(&s_counter_mutex);
    }
    return nullptr;
}

void test_mutexes()
{
    pthread_t threads[contention_threads];
    for (auto& thread : threads)
        CHECK(pthread_create(&thread, nullptr, count_up, nullptr) == 0);
    for (auto& thread : threads)
        pthread_join(thread, nullptr);
    CHECK(s_counter == contention_threads * contention_rounds);

    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    CHECK(pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) == ENOTSUP);
    CHECK(pthread_mutexattr_settype(&attributes, 99) == EINVAL);

    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t recursive;
    pthread_mutex_init(&recursive, &attributes);
    CHECK(pthread_mutex_lock(&recursive) == 0 && pthread_mutex_lock(&recursive) == 0 && pthread_mutex_trylock(&recursive) == 0);
    CHECK(pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0);
    CHECK(pthread_mutex_unlock(&recursive) == EPERM);
    CHECK(pthread_mutex_destroy(&recursive) == 0);

    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t checking;
    pthread_mutex_init(&checking, &attributes);
    CHECK(pthread_mutex_lock(&checking) == 0);
    CHECK(pthread_mutex_lock(&checking) == EDEADLK);
    CHECK(pthread_mutex_trylock(&checking) == EBUSY);
    CHECK(pthread_mutex_destroy(&checking) == EBUSY);
    CHECK(pthread_mutex_unlock(&checking) == 0);
    CHECK(pthread_mutex_unlock(&checking) == EPERM);
    pthread_mutexattr_destroy(&attributes);

    // Held by another thread: a timed lock gives up.
    static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
    static pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, nullptr, 2);
    pthread_t holder;
    pthread_create(&holder, nullptr, [](void*) -> void* {
        pthread_mutex_lock(&held);
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
        pthread_mutex_unlock(&held);
        return nullptr;
    },
        nullptr);
    pthread_barrier_wait(&barrier);
    CHECK(pthread_mutex_trylock(&held) == EBUSY);
    struct timespec when;
    clock_gettime(CLOCK_REALTIME, &when);
    when.tv_nsec += 20'000'000;
    if (when.tv_nsec >= 1'000'000'000) {
        when.tv_nsec -= 1'000'000'000;
        ++when.tv_sec;
    }
    long started = now_microseconds();
    CHECK(pthread_mutex_timedlock(&held, &when) == ETIMEDOUT);
    long took = now_microseconds() - started;
    CHECK(took >= 15'000 && took < 2'000'000);
    pthread_barrier_wait(&barrier);
    pthread_join(holder, nullptr);
    pthread_barrier_destroy(&barrier);
    CHECK(pthread_mutex_lock(&held) == 0 && pthread_mutex_unlock(&held) == 0);

    pthread_spinlock_t spin;
    CHECK(pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) == 0 && pthread_spin_lock(&spin) == 0);
    CHECK(pthread_spin_trylock(&spin) == EBUSY);
    CHECK(pthread_spin_unlock(&spin) == 0 && pthread_spin_destroy(&spin) == 0);
}

// --- Condition variables: a bounded queue, several on each side ---

constexpr int queue_capacity = 4;
constexpr int items_per_producer = 5000;
constexpr int producers = 4;

struct {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
    pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
    long items[queue_capacity] {};
    int count { 0 };
    long consumed_total { 0 };
    int consumed { 0 };
} s_queue;

void* produce(void*)
{
    for (int i = 1; i <= items_per_producer; ++i) {
        pthread_mutex_lock(&s_queue.mutex);
        while (s_queue.count == queue_capacity)
            pthread_cond_wait(&s_queue.not_full, &s_queue.mutex);
        s_queue.items[s_queue.count++] = i;
        pthread_cond_signal(&s_queue.not_empty);
        pthread_mutex_unlock(&s_queue.mutex);
    }
    return nullptr;
}

void* consume(void*)
{
    for (;;) {
        pthread_mutex_lock(&s_queue.mutex);
        while (s_queue.count == 0 && s_queue.consumed < producers * items_per_producer)
            pthread_cond_wait(&s_queue.not_empty, &s_queue.mutex);
        if (s_queue.consumed == producers * items_per_producer) {
            pthread_cond_broadcast(&s_queue.not_empty);
            pthread_mutex_unlock(&s_queue.mutex);
            return nullptr;
        }
        s_queue.consumed_total += s_queue.items[--s_queue.count];
        if (++s_queue.consumed == producers * items_per_producer)
            pthread_cond_broadcast(&s_queue.not_empty);
        pthread_cond_signal(&s_queue.not_full);
        pthread_mutex_unlock(&s_queue.mutex);
    }
}

void test_condition_variables()
{
    pthread_t threads[producers + 3];
    for (int i = 0; i < producers; ++i)
        CHECK(pthread_create(&threads[i], nullptr, produce, nullptr) == 0);
    for (int i = producers; i < producers + 3; ++i)
        CHECK(pthread_create(&threads[i], nullptr, consume, nullptr) == 0);
    for (auto& thread : threads)
        pthread_join(thread, nullptr);
    long expected = static_cast<long>(items_per_producer) * (items_per_producer + 1) / 2 * producers;
    CHECK(s_queue.consumed_total == expected);

    // Timing out, on either clock, with the mutex held afterwards.
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&mutex);
    struct timespec when;
    clock_gettime(CLOCK_REALTIME, &when);
    ++when.tv_sec;
    when.tv_sec -= 1;
    when.tv_nsec += 10'000'000;
    if (when.tv_nsec >= 1'000'000'000) {
        when.tv_nsec -= 1'000'000'000;
        ++when.tv_sec;
    }
    int result;
    do
        result = pthread_cond_timedwait(&condition, &mutex, &when);
    while (result == 0);
    CHECK(result == ETIMEDOUT);
    CHECK(pthread_mutex_trylock(&mutex) == EBUSY);
    struct timespec bad = when;
    bad.tv_nsec = 2'000'000'000;
    CHECK(pthread_cond_timedwait(&condition, &mutex, &bad) == EINVAL);
    pthread_mutex_unlock(&mutex);

    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    CHECK(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
    pthread_cond_t monotonic;
    pthread_cond_init(&monotonic, &attributes);
    pthread_mutex_lock(&mutex);
    clock_gettime(CLOCK_MONOTONIC, &when);
    when.tv_nsec += 10'000'000;
    if (when.tv_nsec >= 1'000'000'000) {
        when.tv_nsec -= 1'000'000'000;
        ++when.tv_sec;
    }
    do
        result = pthread_cond_timedwait(&monotonic, &mutex, &when);
    while (result == 0);
    CHECK(result == ETIMEDOUT);
    pthread_mutex_unlock(&mutex);
    pthread_cond_destroy(&monotonic);
    pthread_condattr_destroy(&attributes);

    // A recursive mutex is released all the way while waiting.
    static pthread_mutex_t recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    static pthread_cond_t go = PTHREAD_COND_INITIALIZER;
    static bool ready = false;
    pthread_mutex_lock(&recursive);
    pthread_mutex_lock(&recursive);
    pthread_t helper;
    pthread_create(&helper, nullptr, [](void*) -> void* {
        pthread_mutex_lock(&recursive);
        ready = true;
        pthread_cond_signal(&go);
        pthread_mutex_unlock(&recursive);
        return nullptr;
    },
        nullptr);
    while (!ready)
        pthread_cond_wait(&go, &recursive);
    CHECK(pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0);
    CHECK(pthread_mutex_unlock(&recursive) == EPERM);
    pthread_join(helper, nullptr);
}

// --- Read-write locks, barriers, once, semaphores ---

pthread_rwlock_t s_rwlock = PTHREAD_RWLOCK_INITIALIZER;
long s_guarded[2];

void* rwlock_user(void* argument)
{
    long writer = reinterpret_cast<long>(argument);
    long mismatches = 0;
    for (int i = 0; i < 5000; ++i) {
        if (writer) {
            pthread_rwlock_wrlock(&s_rwlock);
            ++s_guarded[0];
            ++s_guarded[1];
            pthread_rwlock_unlock(&s_rwlock);
        } else {
            pthread_rwlock_rdlock(&s_rwlock);
            if (s_guarded[0] != s_guarded[1])
                ++mismatches;
            pthread_rwlock_unlock(&s_rwlock);
        }
    }
    return reinterpret_cast<void*>(mismatches);
}

int s_once_runs = 0;
pthread_once_t s_once = PTHREAD_ONCE_INIT;

void* once_user(void*)
{
    pthread_once(&s_once, [] {
        pause_milliseconds(5);
        ++s_once_runs;
    });
    return reinterpret_cast<void*>(static_cast<long>(s_once_runs));
}

// A static several threads meet at once: constructed by one, seen whole by all.
int s_static_constructions = 0;
struct SlowToMake {
    int value;
    SlowToMake()
    {
        __atomic_add_fetch(&s_static_constructions, 1, __ATOMIC_SEQ_CST);
        pause_milliseconds(5);
        value = 77;
    }
};

void* static_user(void*)
{
    static SlowToMake made;
    return reinterpret_cast<void*>(static_cast<long>(made.value));
}

sem_t s_semaphore;

void* semaphore_poster(void*)
{
    for (int i = 0; i < 1000; ++i)
        sem_post(&s_semaphore);
    return nullptr;
}

void* semaphore_taker(void*)
{
    for (int i = 0; i < 1000; ++i) {
        while (sem_wait(&s_semaphore) < 0 && errno == EINTR)
            ;
    }
    return nullptr;
}

void test_other_synchronization()
{
    pthread_t threads[8];
    for (long i = 0; i < 8; ++i)
        CHECK(pthread_create(&threads[i], nullptr, rwlock_user, reinterpret_cast<void*>(i < 2 ? 1l : 0l)) == 0);
    for (auto& thread : threads) {
        void* mismatches = nullptr;
        pthread_join(thread, &mismatches);
        CHECK(mismatches == nullptr);
    }
    CHECK(s_guarded[0] == 10000 && s_guarded[1] == 10000);

    // Read locks nest; a write lock excludes; errors are reported.
    pthread_rwlock_t lock;
    CHECK(pthread_rwlock_init(&lock, nullptr) == 0);
    CHECK(pthread_rwlock_rdlock(&lock) == 0 && pthread_rwlock_rdlock(&lock) == 0);
    CHECK(pthread_rwlock_trywrlock(&lock) == EBUSY);
    CHECK(pthread_rwlock_destroy(&lock) == EBUSY);
    CHECK(pthread_rwlock_unlock(&lock) == 0 && pthread_rwlock_unlock(&lock) == 0);
    CHECK(pthread_rwlock_unlock(&lock) == EPERM);
    CHECK(pthread_rwlock_wrlock(&lock) == 0);
    CHECK(pthread_rwlock_tryrdlock(&lock) == EBUSY);
    CHECK(pthread_rwlock_rdlock(&lock) == EDEADLK && pthread_rwlock_wrlock(&lock) == EDEADLK);
    CHECK(pthread_rwlock_unlock(&lock) == 0 && pthread_rwlock_destroy(&lock) == 0);

    // A barrier: everyone leaves together, one of them serially, repeatedly.
    static pthread_barrier_t barrier;
    static int serial_count = 0;
    static int arrived = 0;
    static int early_leavers = 0;
    CHECK(pthread_barrier_init(&barrier, nullptr, 0) == EINVAL);
    CHECK(pthread_barrier_init(&barrier, nullptr, 6) == 0);
    pthread_t waiters[6];
    for (auto& thread : waiters) {
        CHECK(pthread_create(&thread, nullptr, [](void*) -> void* {
            for (int round = 1; round <= 200; ++round) {
                __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
                if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD)
                    __atomic_add_fetch(&serial_count, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < round * 6)
                    __atomic_add_fetch(&early_leavers, 1, __ATOMIC_SEQ_CST);
            }
            return nullptr;
        },
                  nullptr)
            == 0);
    }
    for (auto& thread : waiters)
        pthread_join(thread, nullptr);
    CHECK(serial_count == 200 && early_leavers == 0);
    pthread_barrier_destroy(&barrier);

    for (auto& thread : threads)
        CHECK(pthread_create(&thread, nullptr, once_user, nullptr) == 0);
    for (auto& thread : threads) {
        void* runs = nullptr;
        pthread_join(thread, &runs);
        CHECK(reinterpret_cast<long>(runs) == 1);
    }
    CHECK(s_once_runs == 1);

    for (auto& thread : threads)
        CHECK(pthread_create(&thread, nullptr, static_user, nullptr) == 0);
    for (auto& thread : threads) {
        void* value = nullptr;
        pthread_join(thread, &value);
        CHECK(reinterpret_cast<long>(value) == 77);
    }
    CHECK(s_static_constructions == 1);

    CHECK(sem_init(&s_semaphore, 0, 2) == 0);
    int value = 0;
    CHECK(sem_getvalue(&s_semaphore, &value) == 0 && value == 2);
    CHECK(sem_trywait(&s_semaphore) == 0 && sem_trywait(&s_semaphore) == 0);
    CHECK(sem_trywait(&s_semaphore) == -1 && errno == EAGAIN);
    struct timespec when;
    clock_gettime(CLOCK_REALTIME, &when);
    when.tv_nsec += 5'000'000;
    if (when.tv_nsec >= 1'000'000'000) {
        when.tv_nsec -= 1'000'000'000;
        ++when.tv_sec;
    }
    CHECK(sem_timedwait(&s_semaphore, &when) == -1 && errno == ETIMEDOUT);
    for (int i = 0; i < 4; ++i)
        CHECK(pthread_create(&threads[i], nullptr, semaphore_taker, nullptr) == 0);
    for (int i = 4; i < 8; ++i)
        CHECK(pthread_create(&threads[i], nullptr, semaphore_poster, nullptr) == 0);
    for (auto& thread : threads)
        pthread_join(thread, nullptr);
    CHECK(sem_getvalue(&s_semaphore, &value) == 0 && value == 0);
    CHECK(sem_destroy(&s_semaphore) == 0);
}

// --- Keys and cleanup handlers ---

int s_key_destructions = 0;
void note_destruction(void* value)
{
    if (reinterpret_cast<long>(value) == 5)
        __atomic_add_fetch(&s_key_destructions, 1, __ATOMIC_SEQ_CST);
}

pthread_key_t s_key;
int s_rearmed = 0;
pthread_key_t s_rearming_key;

void rearm(void*)
{
    // A destructor may set a value: it is called again, a limited number of times.
    if (++s_rearmed < 2)
        pthread_setspecific(s_rearming_key, reinterpret_cast<void*>(1));
}

int s_cleanup_order[3];
int s_cleanup_count = 0;
void note_cleanup(void* argument)
{
    s_cleanup_order[s_cleanup_count++] = static_cast<int>(reinterpret_cast<long>(argument));
}

void test_keys_and_cleanup()
{
    CHECK(pthread_key_create(&s_key, note_destruction) == 0);
    CHECK(pthread_key_create(&s_rearming_key, rearm) == 0);
    CHECK(pthread_getspecific(s_key) == nullptr);
    CHECK(pthread_setspecific(s_key, reinterpret_cast<void*>(3)) == 0);
    pthread_t threads[4];
    for (auto& thread : threads) {
        CHECK(pthread_create(&thread, nullptr, [](void*) -> void* {
            if (pthread_getspecific(s_key) != nullptr)
                return reinterpret_cast<void*>(1);
            pthread_setspecific(s_key, reinterpret_cast<void*>(5));
            return nullptr;
        },
                  nullptr)
            == 0);
    }
    for (auto& thread : threads) {
        void* failed = nullptr;
        pthread_join(thread, &failed);
        CHECK(failed == nullptr);
    }
    CHECK(s_key_destructions == 4);
    CHECK(reinterpret_cast<long>(pthread_getspecific(s_key)) == 3);

    pthread_t rearming;
    pthread_create(&rearming, nullptr, [](void*) -> void* {
        pthread_setspecific(s_rearming_key, reinterpret_cast<void*>(1));
        return nullptr;
    },
        nullptr);
    pthread_join(rearming, nullptr);
    CHECK(s_rearmed == 2);

    // A key used again starts out empty.
    pthread_key_t old_key = s_key;
    CHECK(pthread_key_delete(s_key) == 0);
    CHECK(pthread_key_delete(s_key) == EINVAL);
    CHECK(pthread_setspecific(s_key, nullptr) == EINVAL);
    CHECK(pthread_key_create(&s_key, nullptr) == 0);
    CHECK(s_key == old_key);
    CHECK(pthread_getspecific(s_key) == nullptr);
    pthread_key_delete(s_key);
    pthread_key_delete(s_rearming_key);

    pthread_t cleaner;
    pthread_create(&cleaner, nullptr, [](void*) -> void* {
        pthread_cleanup_push(note_cleanup, reinterpret_cast<void*>(1));
        pthread_cleanup_push(note_cleanup, reinterpret_cast<void*>(2));
        pthread_cleanup_push(note_cleanup, reinterpret_cast<void*>(3));
        pthread_cleanup_pop(0);
        pthread_exit(nullptr);
        pthread_cleanup_pop(0);
        pthread_cleanup_pop(0);
        return nullptr;
    },
        nullptr);
    pthread_join(cleaner, nullptr);
    CHECK(s_cleanup_count == 2 && s_cleanup_order[0] == 2 && s_cleanup_order[1] == 1);
}

// --- The C library under fire ---

void* hammer_malloc(void* argument)
{
    unsigned seed = static_cast<unsigned>(reinterpret_cast<long>(argument)) * 2654435761u + 1;
    void* blocks[64] = {};
    size_t sizes[64] = {};
    long failures = 0;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245 + 12345;
        unsigned slot = (seed >> 16) % 64;
        if (blocks[slot]) {
            auto* bytes = static_cast<unsigned char*>(blocks[slot]);
            for (size_t j = 0; j < sizes[slot]; j += 97) {
                if (bytes[j] != static_cast<unsigned char>(slot))
                    ++failures;
            }
            free(blocks[slot]);
            blocks[slot] = nullptr;
        } else {
            sizes[slot] = 1 + (seed >> 8) % ((seed & 0xf) == 0 ? 40000 : 800);
            blocks[slot] = malloc(sizes[slot]);
            if (!blocks[slot]) {
                ++failures;
                continue;
            }
            memset(blocks[slot], static_cast<int>(slot), sizes[slot]);
        }
    }
    for (auto* block : blocks)
        free(block);
    return reinterpret_cast<void*>(failures);
}

FILE* s_shared_file;

void* write_lines(void* argument)
{
    long who = reinterpret_cast<long>(argument);
    for (int i = 0; i < 1000; ++i)
        fprintf(s_shared_file, "<%ld:%04d:%s>\n", who, i, "the whole line goes together");
    // Locked by hand, several calls are one.
    flockfile(s_shared_file);
    fputs("<first half ", s_shared_file);
    pause_milliseconds(1);
    fputs("second half>\n", s_shared_file);
    funlockfile(s_shared_file);
    return nullptr;
}

void test_c_library()
{
    pthread_t threads[8];
    for (long i = 0; i < 8; ++i)
        CHECK(pthread_create(&threads[i], nullptr, hammer_malloc, reinterpret_cast<void*>(i)) == 0);
    for (auto& thread : threads) {
        void* failures = nullptr;
        pthread_join(thread, &failures);
        CHECK(failures == nullptr);
    }

    s_shared_file = tmpfile();
    CHECK(s_shared_file != nullptr);
    for (long i = 0; i < 4; ++i)
        CHECK(pthread_create(&threads[i], nullptr, write_lines, reinterpret_cast<void*>(i)) == 0);
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], nullptr);
    rewind(s_shared_file);
    int counts[4] = {};
    int halves = 0;
    int bad_lines = 0;
    char line[128];
    while (fgets(line, sizeof(line), s_shared_file)) {
        int who = 0, number = 0;
        if (strcmp(line, "<first half second half>\n") == 0)
            ++halves;
        else if (sscanf(line, "<%d:%d:the whole line goes together>\n", &who, &number) == 2 && who >= 0 && who < 4 && number == counts[who])
            ++counts[who];
        else
            ++bad_lines;
    }
    CHECK(bad_lines == 0 && halves == 4);
    CHECK(counts[0] == 1000 && counts[1] == 1000 && counts[2] == 1000 && counts[3] == 1000);
    fclose(s_shared_file);
}

// --- Signals ---

pthread_t volatile s_handled_by;
int volatile s_handled_signal;

void note_handler(int signal)
{
    s_handled_by = pthread_self();
    s_handled_signal = signal;
}

void install(int signal, void (*handler)(int), int flags = 0)
{
    struct sigaction action {};
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
}

bool wait_for_handler(int signal)
{
    for (int i = 0; i < 1000 && s_handled_signal != signal; ++i)
        pause_milliseconds(2);
    return s_handled_signal == signal;
}

void* sleep_until_told(void* argument)
{
    auto* flag = static_cast<int volatile*>(argument);
    while (!*flag)
        pause_milliseconds(1);
    return nullptr;
}

void* spin_until_told(void* argument)
{
    auto* flag = static_cast<int volatile*>(argument);
    while (!*flag)
        pause_briefly();
    return nullptr;
}

void test_signals()
{
    install(SIGUSR1, note_handler);

    // A signal for the process goes to a thread that does not block it, even
    // one that is running flat out in user mode.
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
    static int volatile stop;
    stop = 0;
    pthread_t worker;
    // (It inherits the mask, then lets the signal in.)
    pthread_create(&worker, nullptr, [](void*) -> void* {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigset_t current;
        pthread_sigmask(SIG_BLOCK, nullptr, &current);
        if (!sigismember(&current, SIGUSR1))
            return reinterpret_cast<void*>(1);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
        while (!stop)
            pause_briefly();
        return nullptr;
    },
        nullptr);
    pause_milliseconds(5);
    s_handled_signal = 0;
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_handler(SIGUSR1));
    CHECK(pthread_equal(s_handled_by, worker));
    stop = 1;
    void* inherited_failed = nullptr;
    pthread_join(worker, &inherited_failed);
    CHECK(inherited_failed == nullptr);

    // The thread that kept it from the signal gets it as soon as nobody else can.
    s_handled_signal = 0;
    kill(getpid(), SIGUSR1);
    sigset_t pending;
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1));
    CHECK(s_handled_signal == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0);
    CHECK(s_handled_signal == SIGUSR1 && pthread_equal(s_handled_by, pthread_self()));

    // pthread_kill: that thread and no other.
    stop = 0;
    pthread_t sleeper;
    pthread_create(&sleeper, nullptr, sleep_until_told, const_cast<int*>(&stop));
    s_handled_signal = 0;
    CHECK(pthread_kill(sleeper, SIGUSR1) == 0);
    CHECK(wait_for_handler(SIGUSR1));
    CHECK(pthread_equal(s_handled_by, sleeper));
    CHECK(pthread_kill(sleeper, 0) == 0);
    CHECK(pthread_kill(sleeper, 1000) == EINVAL);
    stop = 1;
    pthread_join(sleeper, nullptr);

    // A pending signal of one thread is not another's.
    static pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, nullptr, 2);
    CHECK(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
    pthread_t pender;
    static sigset_t pender_pending;
    pthread_create(&pender, nullptr, [](void*) -> void* {
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
        sigpending(&pender_pending);
        // Taken by sigwait, which needs no handler.
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        int got = 0;
        sigwait(&set, &got);
        return reinterpret_cast<void*>(static_cast<long>(got));
    },
        nullptr);
    pthread_barrier_wait(&barrier);
    pthread_kill(pender, SIGUSR1);
    CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGUSR1));
    pthread_barrier_wait(&barrier);
    void* got = nullptr;
    pthread_join(pender, &got);
    CHECK(reinterpret_cast<long>(got) == SIGUSR1);
    CHECK(sigismember(&pender_pending, SIGUSR1));
    pthread_barrier_destroy(&barrier);

    // sigtimedwait gives up; sigwait takes a signal for the process, even one
    // ignored by default, while every thread blocks it.
    struct timespec brief { 0, 5'000'000 };
    CHECK(sigtimedwait(&blocked, nullptr, &brief) == -1 && errno == EAGAIN);
    kill(getpid(), SIGUSR1);
    siginfo_t info {};
    CHECK(sigtimedwait(&blocked, &info, &brief) == SIGUSR1 && info.si_signo == SIGUSR1 && info.si_pid == getpid());
    sigset_t winch;
    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &winch, nullptr);
    kill(getpid(), SIGWINCH);
    int taken = 0;
    CHECK(sigwait(&winch, &taken) == 0 && taken == SIGWINCH);
    pthread_sigmask(SIG_UNBLOCK, &winch, nullptr);
    CHECK(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0);

    // raise: the calling thread.
    s_handled_signal = 0;
    raise(SIGUSR1);
    CHECK(s_handled_signal == SIGUSR1 && pthread_equal(s_handled_by, pthread_self()));
    install(SIGUSR1, SIG_DFL);
}

// FP registers survive a handler that uses them, and fork.
void clobber_floating_point(int)
{
#if defined(__x86_64__)
    asm volatile("pxor %%xmm7, %%xmm7\n"
                 "pxor %%xmm8, %%xmm8\n"
                 "pcmpeqd %%xmm15, %%xmm15"
        : : : "xmm7", "xmm8", "xmm15");
#else
    asm volatile("fmov d16, xzr\n"
                 "fmov d8, xzr\n"
                 "movi v31.16b, #0xff"
        : : : "v8", "v16", "v31");
#endif
    s_handled_signal = SIGUSR2;
}

// A raw system call, with `value` in a floating-point register across it, which it returns as `out`.
long syscall_with_value_in_fp_register(Syscall number, unsigned long a0, unsigned long a1, unsigned long value, unsigned long& out)
{
#if defined(__x86_64__)
    unsigned long result = static_cast<unsigned long>(number);
    asm volatile("movq %[value], %%xmm7\n"
                 "syscall\n"
                 "movq %%xmm7, %[out]\n"
        : [out] "=r"(out), "+a"(result)
        : [value] "r"(value), "D"(a0), "S"(a1)
        : "rcx", "r11", "xmm7", "memory");
    return static_cast<long>(result);
#else
    register unsigned long x8 asm("x8") = static_cast<unsigned long>(number);
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    asm volatile("fmov d16, %[value]\n"
                 "svc #0\n"
                 "fmov %[out], d16\n"
        : [out] "=r"(out), "+r"(x0)
        : [value] "r"(value), "r"(x8), "r"(x1)
        : "v16", "memory");
    return static_cast<long>(x0);
#endif
}

void test_floating_point_state()
{
    install(SIGUSR2, clobber_floating_point);
    unsigned long value = 0x1234'5678'9abc'def0;
    unsigned long out = 0;
    s_handled_signal = 0;
    (void)syscall_with_value_in_fp_register(Syscall::thread_kill, static_cast<unsigned long>(gettid()), SIGUSR2, value, out);
    CHECK(s_handled_signal == SIGUSR2);
    CHECK(out == value);
    install(SIGUSR2, SIG_DFL);

    // A raw fork, for what the kernel alone does.
    int status = in_child([&] {
        unsigned long seen = 0;
        long const child = syscall_with_value_in_fp_register(Syscall::fork, 0, 0, value, seen);
        if (child != 0) {
            int inner = 0;
            waitpid(static_cast<pid_t>(child), &inner, 0);
            return WIFEXITED(inner) ? WEXITSTATUS(inner) : 99;
        }
        _exit(seen == value ? 0 : 1);
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// The alternate stack, and surviving a stack overflow on it.
char s_alternate_stack[SIGSTKSZ] __attribute__((aligned(16)));
int volatile s_alternate_stack_used;

void note_stack_handler(int)
{
    int local = 0;
    auto address = reinterpret_cast<char*>(&local);
    s_alternate_stack_used = address >= s_alternate_stack && address < s_alternate_stack + sizeof(s_alternate_stack);
    stack_t now;
    if (sigaltstack(nullptr, &now) != 0 || !(now.ss_flags & SS_ONSTACK))
        s_alternate_stack_used = 0;
    stack_t change {};
    change.ss_flags = SS_DISABLE;
    if (sigaltstack(&change, nullptr) != -1 || errno != EPERM)
        s_alternate_stack_used = 0;
}

[[gnu::noinline]] int overflow(int depth)
{
    volatile char filler[4096];
    filler[0] = static_cast<char>(depth);
    // (Far deeper than any stack here goes.)
    if (depth > 1'000'000'000)
        return 0;
    return overflow(depth + 1) + filler[0];
}

void test_alternate_stack()
{
    stack_t stack {};
    stack_t old {};
    CHECK(sigaltstack(nullptr, &old) == 0 && (old.ss_flags & SS_DISABLE));
    stack.ss_sp = s_alternate_stack;
    stack.ss_size = 100;
    CHECK(sigaltstack(&stack, nullptr) == -1 && errno == ENOMEM);
    stack.ss_size = sizeof(s_alternate_stack);
    stack.ss_flags = 12345;
    CHECK(sigaltstack(&stack, nullptr) == -1 && errno == EINVAL);
    stack.ss_flags = 0;
    CHECK(sigaltstack(&stack, nullptr) == 0);
    CHECK(sigaltstack(nullptr, &old) == 0 && old.ss_sp == s_alternate_stack && old.ss_size == sizeof(s_alternate_stack) && old.ss_flags == 0);
    install(SIGUSR2, note_stack_handler, SA_ONSTACK);
    raise(SIGUSR2);
    CHECK(s_alternate_stack_used == 1);
    install(SIGUSR2, SIG_DFL);

    // Each thread has its own: a new one has none.
    pthread_t thread;
    void* flags = nullptr;
    pthread_create(&thread, nullptr, [](void*) -> void* {
        stack_t mine;
        sigaltstack(nullptr, &mine);
        return reinterpret_cast<void*>(static_cast<long>(mine.ss_flags));
    },
        nullptr);
    pthread_join(thread, &flags);
    CHECK(reinterpret_cast<long>(flags) == SS_DISABLE);
    stack.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&stack, nullptr) == 0);

    // A thread overflowing its stack hits the guard page; the handler, on
    // the alternate stack, is still able to run.
    int status = in_child([] {
        install(SIGSEGV, [](int) {
            static char const message[] = "";
            (void)write(2, message, 0);
            _exit(33);
        }, SA_ONSTACK);
        pthread_t overflower;
        pthread_create(&overflower, nullptr, [](void*) -> void* {
            stack_t own {};
            own.ss_sp = s_alternate_stack;
            own.ss_size = sizeof(s_alternate_stack);
            sigaltstack(&own, nullptr);
            return reinterpret_cast<void*>(static_cast<long>(overflow(0)));
        },
            nullptr);
        pthread_join(overflower, nullptr);
        return 1;
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 33);
}

// --- The process as a whole ---

void test_process_ends_with_threads()
{
    // exit() from any thread ends all of them: running, sleeping, waiting on
    // a futex, in a read, in poll, stopped at a barrier.
    int status = in_child([] {
        static int fds[2];
        (void)pipe(fds);
        static pthread_barrier_t never;
        pthread_barrier_init(&never, nullptr, 100);
        static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
        static int volatile never_set = 0;
        pthread_t thread;
        pthread_create(&thread, nullptr, spin_until_told, const_cast<int*>(&never_set));
        pthread_create(&thread, nullptr, [](void*) -> void* {
            for (;;)
                pause_milliseconds(100000);
        },
            nullptr);
        pthread_create(&thread, nullptr, [](void*) -> void* {
            pthread_barrier_wait(&never);
            return nullptr;
        },
            nullptr);
        pthread_create(&thread, nullptr, [](void*) -> void* {
            char byte;
            (void)read(fds[0], &byte, 1);
            return nullptr;
        },
            nullptr);
        pthread_create(&thread, nullptr, [](void*) -> void* {
            pollfd watched { fds[0], POLLIN, 0 };
            poll(&watched, 1, -1);
            return nullptr;
        },
            nullptr);
        pthread_create(&thread, nullptr, [](void*) -> void* {
            pthread_mutex_lock(&mutex);
            pthread_cond_wait(&condition, &mutex);
            return nullptr;
        },
            nullptr);
        pause_milliseconds(10);
        pthread_create(&thread, nullptr, [](void*) -> void* {
            exit(7);
        },
            nullptr);
        for (;;)
            pause_milliseconds(1000);
        return 0;
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);

    // SIGKILL ends a process whose only thread never enters the kernel.
    pid_t spinner = fork();
    if (spinner == 0) {
        for (;;)
            pause_briefly();
    }
    pause_milliseconds(10);
    long started = now_microseconds();
    kill(spinner, SIGKILL);
    status = wait_for(spinner);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(now_microseconds() - started < 1'000'000);

    // The main thread may go first; the last one ends the process as exit(0)
    // would, atexit handlers and all.
    static int fds[2];
    CHECK(pipe(fds) == 0);
    status = in_child([]() -> int {
        atexit([] { (void)write(fds[1], "x", 1); });
        pthread_t worker;
        pthread_create(&worker, nullptr, [](void*) -> void* {
            pause_milliseconds(10);
            (void)write(fds[1], "w", 1);
            return nullptr;
        },
            nullptr);
        pthread_exit(nullptr);
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char bytes[3] = {};
    CHECK(read(fds[0], bytes, 2) == 2 && strcmp(bytes, "wx") == 0);
    close(fds[0]);
    close(fds[1]);

    // A thread can join the main thread, and gets what it left.
    status = in_child([]() -> int {
        static pthread_t main_thread;
        main_thread = pthread_self();
        pthread_t worker;
        pthread_create(&worker, nullptr, [](void*) -> void* {
            void* result = nullptr;
            if (pthread_join(main_thread, &result) != 0)
                exit(2);
            exit(reinterpret_cast<long>(result) == 55 ? 21 : 3);
        },
            nullptr);
        pause_milliseconds(5);
        pthread_exit(reinterpret_cast<void*>(55));
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 21);

    // A fault in any thread is fatal to the process (it has a crash report).
    status = in_child([] {
        pthread_t worker;
        static int volatile never_set = 0;
        pthread_create(&worker, nullptr, spin_until_told, const_cast<int*>(&never_set));
        pthread_create(&worker, nullptr, [](void*) -> void* {
            *reinterpret_cast<int volatile*>(8) = 1;
            return nullptr;
        },
            nullptr);
        pthread_join(worker, nullptr);
        return 1;
    });
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    // A default-fatal signal for the process also takes every thread.
    pid_t victim = fork();
    if (victim == 0) {
        static int volatile never_set = 0;
        pthread_t worker;
        pthread_create(&worker, nullptr, spin_until_told, const_cast<int*>(&never_set));
        pthread_create(&worker, nullptr, sleep_until_told, const_cast<int*>(&never_set));
        for (;;)
            pause();
    }
    pause_milliseconds(10);
    kill(victim, SIGTERM);
    status = wait_for(victim);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    // exec from a thread: the other threads are gone, the process is the same.
    status = in_child([] {
        static int volatile never_set = 0;
        pthread_t worker;
        pthread_create(&worker, nullptr, spin_until_told, const_cast<int*>(&never_set));
        pthread_create(&worker, nullptr, sleep_until_told, const_cast<int*>(&never_set));
        pthread_create(&worker, nullptr, [](void*) -> void* {
            char expected[32];
            snprintf(expected, sizeof(expected), "exit $(($$ == %d ? 42 : 43))", getpid());
            execl("/bin/sh", "sh", "-c", expected, nullptr);
            _exit(127);
        },
            nullptr);
        for (;;)
            pause_milliseconds(1000);
        return 0;
    });
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
}

void test_fork_with_threads()
{
    // While others hammer malloc and stdio, a fork from any thread has a child
    // of one thread, in which they work: no lock is left held by a thread that
    // is not there.
    static int volatile stop;
    stop = 0;
    pthread_t hammers[4];
    for (auto& thread : hammers) {
        CHECK(pthread_create(&thread, nullptr, [](void*) -> void* {
            while (!stop) {
                void* block = malloc(128);
                FILE* file = tmpfile();
                if (file) {
                    fputs("x", file);
                    fclose(file);
                }
                free(block);
            }
            return nullptr;
        },
                  nullptr)
            == 0);
    }
    pause_milliseconds(2);
    static int prepared, parents, children;
    CHECK(pthread_atfork([] { ++prepared; }, [] { ++parents; }, [] { ++children; }) == 0);
    for (int i = 0; i < 40; ++i) {
        pid_t child = fork();
        if (child == 0) {
            if (children != 1 || parents != i)
                _exit(4);
            if (thread_count() != 1)
                _exit(3);
            void* block = malloc(4096);
            FILE* file = tmpfile();
            if (!block || !file)
                _exit(2);
            fprintf(file, "%d", i);
            fclose(file);
            free(block);
            // It may make threads of its own.
            pthread_t thread;
            void* result = nullptr;
            if (pthread_create(&thread, nullptr, add_one, reinterpret_cast<void*>(1)) != 0 || pthread_join(thread, &result) != 0
                || reinterpret_cast<long>(result) != 2)
                _exit(5);
            _exit(0);
        }
        int status = wait_for(child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(prepared == 40 && parents == 40);
    stop = 1;
    for (auto& thread : hammers)
        pthread_join(thread, nullptr);

    // Forking from a thread that is not the first.
    pthread_t forker;
    pthread_create(&forker, nullptr, [](void*) -> void* {
        pid_t child = fork();
        if (child == 0) {
            // It is the first thread now.
            _exit(pthread_main_np() && thread_count() == 1 ? 11 : 12);
        }
        return reinterpret_cast<void*>(static_cast<long>(wait_for(child)));
    },
        nullptr);
    void* result = nullptr;
    pthread_join(forker, &result);
    int status = static_cast<int>(reinterpret_cast<long>(result));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 11);
}

// Stopping stops every thread; continuing continues them.
void test_stop_and_continue()
{
    char path[] = "/tmp/threads-test.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unlink(path);
    CHECK(ftruncate(fd, 4096) == 0);
    auto* counters = static_cast<long volatile*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    CHECK(counters != MAP_FAILED);
    pid_t child = fork();
    if (child == 0) {
        static long volatile* shared;
        shared = counters;
        for (long i = 1; i < 4; ++i) {
            pthread_t worker;
            pthread_create(&worker, nullptr, [](void* argument) -> void* {
                long slot = reinterpret_cast<long>(argument);
                for (;;)
                    __atomic_add_fetch(const_cast<long*>(&shared[slot]), 1, __ATOMIC_RELAXED);
            },
                reinterpret_cast<void*>(i));
        }
        for (;;)
            __atomic_add_fetch(const_cast<long*>(&shared[0]), 1, __ATOMIC_RELAXED);
    }
    auto read_all = [&](long* into) {
        for (int i = 0; i < 4; ++i)
            into[i] = __atomic_load_n(const_cast<long*>(&counters[i]), __ATOMIC_RELAXED);
    };
    long before[4], after[4];
    for (int i = 0; i < 1000; ++i) {
        read_all(before);
        if (before[0] && before[1] && before[2] && before[3])
            break;
        pause_milliseconds(1);
    }
    CHECK(before[0] && before[1] && before[2] && before[3]);

    CHECK(kill(child, SIGSTOP) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    pause_milliseconds(10);
    read_all(before);
    pause_milliseconds(30);
    read_all(after);
    for (int i = 0; i < 4; ++i)
        CHECK(before[i] == after[i]);

    CHECK(kill(child, SIGCONT) == 0);
    pause_milliseconds(30);
    read_all(after);
    for (int i = 0; i < 4; ++i)
        CHECK(after[i] > before[i]);

    // And SIGKILL ends it even while stopped.
    CHECK(kill(child, SIGSTOP) == 0);
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    CHECK(kill(child, SIGKILL) == 0);
    status = wait_for(child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    munmap(const_cast<long*>(counters), 4096);
    close(fd);
}

// The futex itself.
void test_futex()
{
    unsigned word = 1;
    auto wait = [&](unsigned expected, timespec const* timeout) {
        return System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&word), VULTURE_FUTEX_WAIT, expected, reinterpret_cast<unsigned long>(timeout));
    };
    timespec brief { 0, 5'000'000 };
    CHECK(wait(2, nullptr) == -EAGAIN);
    long started = now_microseconds();
    CHECK(wait(1, &brief) == -ETIMEDOUT);
    long took = now_microseconds() - started;
    CHECK(took >= 4'000 && took < 1'000'000);
    timespec bad { 0, 2'000'000'000 };
    CHECK(wait(1, &bad) == -EINVAL);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&word) + 1, VULTURE_FUTEX_WAIT, 1, 0) == -EINVAL);
    CHECK(System::syscall(Syscall::futex, 0, VULTURE_FUTEX_WAIT, 1, 0) == -EINVAL);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&word), 99, 1, 0) == -EINVAL);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&word), VULTURE_FUTEX_WAKE, 10, 0) == 0);

    // Waking wakes the number asked for, and only waiters of that word.
    static unsigned shared_word;
    static unsigned other_word;
    static long woken;
    pthread_t waiters[4];
    for (auto& thread : waiters) {
        pthread_create(&thread, nullptr, [](void*) -> void* {
            System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&shared_word), VULTURE_FUTEX_WAIT, 0, 0);
            __atomic_add_fetch(&woken, 1, __ATOMIC_SEQ_CST);
            return nullptr;
        },
            nullptr);
    }
    pause_milliseconds(20);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&other_word), VULTURE_FUTEX_WAKE, 100, 0) == 0);
    pause_milliseconds(5);
    CHECK(__atomic_load_n(&woken, __ATOMIC_SEQ_CST) == 0);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&shared_word), VULTURE_FUTEX_WAKE, 3, 0) == 3);
    for (int i = 0; i < 1000 && __atomic_load_n(&woken, __ATOMIC_SEQ_CST) != 3; ++i)
        pause_milliseconds(1);
    CHECK(__atomic_load_n(&woken, __ATOMIC_SEQ_CST) == 3);
    CHECK(System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&shared_word), VULTURE_FUTEX_WAKE, 3, 0) == 1);
    for (auto& thread : waiters)
        pthread_join(thread, nullptr);
    CHECK(woken == 4);

    // A signal ends a wait, with EINTR.
    install(SIGUSR1, note_handler);
    static unsigned quiet_word;
    static long interrupted;
    pthread_t waiter;
    pthread_create(&waiter, nullptr, [](void*) -> void* {
        interrupted = System::syscall(Syscall::futex, reinterpret_cast<unsigned long>(&quiet_word), VULTURE_FUTEX_WAIT, 0, 0);
        return nullptr;
    },
        nullptr);
    pause_milliseconds(10);
    pthread_kill(waiter, SIGUSR1);
    pthread_join(waiter, nullptr);
    CHECK(interrupted == -EINTR);
    install(SIGUSR1, SIG_DFL);
}

// What sits above: event loops per thread, messages between threads.
void test_libraries_above()
{
    static IPC::Port server_send;
    auto ports = IPC::create_port();
    CHECK(!ports.is_error());
    if (ports.is_error())
        return;
    server_send = move(ports.value().send);
    static IPC::Port server_receive;
    server_receive = move(ports.value().receive);

    pthread_t server;
    static int volatile server_ready;
    pthread_create(&server, nullptr, [](void*) -> void* {
        // A loop of this thread's own, beside whichever others run.
        Core::EventLoop loop;
        IPC::Listener listener(loop, move(server_receive), [&](IPC::ReceivedMessage& message) {
            if (message.id == 2) {
                loop.quit(0);
                return;
            }
            IPC::Encoder answer(0x1234, message.id | IPC::answer_bit);
            IPC::Decoder decoder(message);
            u32 magic = 0, value = 0;
            if (!decoder.decode(magic).is_error() && !decoder.decode(value).is_error())
                answer.encode(value + 1);
            (void)IPC::answer(message, answer);
        });
        server_ready = 1;
        loop.exec();
        return nullptr;
    },
        nullptr);
    while (!server_ready)
        pause_milliseconds(1);

    // Calls from several threads at once get their own answers.
    pthread_t callers[6];
    for (auto& thread : callers) {
        CHECK(pthread_create(&thread, nullptr, [](void*) -> void* {
            long failures = 0;
            for (u32 i = 0; i < 2000; ++i) {
                IPC::Encoder request(0x1234, 1);
                request.encode(i);
                auto answer = IPC::call(server_send, request);
                if (answer.is_error()) {
                    ++failures;
                    continue;
                }
                IPC::Decoder decoder(answer.value());
                u32 magic = 0, value = 0;
                if (decoder.decode(magic).is_error() || decoder.decode(value).is_error() || value != i + 1)
                    ++failures;
            }
            return reinterpret_cast<void*>(failures);
        },
                  nullptr)
            == 0);
    }
    for (auto& thread : callers) {
        void* failures = nullptr;
        pthread_join(thread, &failures);
        CHECK(failures == nullptr);
    }
    IPC::Encoder stop(0x1234, 2);
    CHECK(!IPC::send(server_send, stop).is_error());
    pthread_join(server, nullptr);
    server_send = IPC::Port();

    // And this thread has no loop of its own unless it makes one.
    {
        Core::EventLoop mine;
        bool fired = false;
        mine.add_timer(1, false, [&] {
            fired = true;
            mine.quit(0);
        });
        mine.exec();
        CHECK(fired);
    }
}

void report_timing()
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    constexpr int rounds = 100000;
    long started = now_microseconds();
    for (int i = 0; i < rounds; ++i) {
        pthread_mutex_lock(&mutex);
        pthread_mutex_unlock(&mutex);
    }
    long lock_time = (now_microseconds() - started) * 1000 / rounds;

    started = now_microseconds();
    constexpr int creations = 500;
    for (int i = 0; i < creations; ++i) {
        pthread_t thread;
        pthread_create(&thread, nullptr, add_one, nullptr);
        pthread_join(thread, nullptr);
    }
    long create_time = (now_microseconds() - started) / creations;
    printf("threads: an uncontended mutex lock and unlock: %ld ns; pthread_create and pthread_join: %ld us\n", lock_time, create_time);
    fflush(stdout);
}

}

int main()
{
    test_thread_local_storage();
    test_create_and_join();
    test_mutexes();
    test_condition_variables();
    test_other_synchronization();
    test_keys_and_cleanup();
    test_c_library();
    test_signals();
    test_floating_point_state();
    test_alternate_stack();
    test_futex();
    test_process_ends_with_threads();
    test_fork_with_threads();
    test_stop_and_continue();
    test_libraries_above();
    report_timing();
    CHECK(thread_count() == 1);
    return finish();
}
