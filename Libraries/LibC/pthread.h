/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <signal.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

__BEGIN_DECLS

typedef struct vulture_pthread* pthread_t;
typedef unsigned pthread_key_t;

#define PTHREAD_KEYS_MAX 512
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_STACK_MIN 16384

#define PTHREAD_CREATE_JOINABLE 1
#define PTHREAD_CREATE_DETACHED 2

#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_PROCESS_PRIVATE 2

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

typedef struct {
    int detach_state;
    int reserved;
    size_t stack_size;
    size_t guard_size;
    void* stack_address;
} pthread_attr_t;

typedef struct {
    unsigned state;
    int type;
    void* owner;
    unsigned count;
    unsigned reserved;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

typedef struct {
    unsigned sequence;
    unsigned waiters;
    clockid_t clock;
    unsigned reserved;
} pthread_cond_t;

typedef struct {
    clockid_t clock;
} pthread_condattr_t;

typedef struct {
    unsigned state;
    unsigned waiters;
    void* writer;
} pthread_rwlock_t;

typedef struct {
    int reserved;
} pthread_rwlockattr_t;

typedef struct {
    unsigned state;
} pthread_once_t;

typedef struct {
    unsigned count;
    unsigned waiting;
    unsigned generation;
    unsigned lock;
} pthread_barrier_t;

typedef struct {
    int reserved;
} pthread_barrierattr_t;

typedef volatile int pthread_spinlock_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_NORMAL, 0, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER { 0, 0, 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, 0 }
#define PTHREAD_ONCE_INIT { 0 }

// Threads.
int pthread_create(pthread_t*, pthread_attr_t const*, void* (*start_routine)(void*), void* argument);
int pthread_join(pthread_t, void** result);
int pthread_detach(pthread_t);
[[noreturn]] void pthread_exit(void* result);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
int pthread_kill(pthread_t, int signal);
int pthread_sigmask(int how, sigset_t const* set, sigset_t* old_set);
int pthread_once(pthread_once_t*, void (*routine)(void));
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

int pthread_attr_init(pthread_attr_t*);
int pthread_attr_destroy(pthread_attr_t*);
int pthread_attr_getdetachstate(pthread_attr_t const*, int*);
int pthread_attr_setdetachstate(pthread_attr_t*, int);
int pthread_attr_getstacksize(pthread_attr_t const*, size_t*);
int pthread_attr_setstacksize(pthread_attr_t*, size_t);
int pthread_attr_getguardsize(pthread_attr_t const*, size_t*);
int pthread_attr_setguardsize(pthread_attr_t*, size_t);
int pthread_attr_getstack(pthread_attr_t const*, void** address, size_t* size);
int pthread_attr_setstack(pthread_attr_t*, void* address, size_t size);

// macOS's additions.
int pthread_threadid_np(pthread_t, __UINT64_TYPE__* id);
int pthread_setname_np(char const* name);
int pthread_getname_np(pthread_t, char* name, size_t size);
int pthread_main_np(void);
void* pthread_get_stackaddr_np(pthread_t);
size_t pthread_get_stacksize_np(pthread_t);
void pthread_yield_np(void);

// Mutexes.
int pthread_mutex_init(pthread_mutex_t*, pthread_mutexattr_t const*);
int pthread_mutex_destroy(pthread_mutex_t*);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_trylock(pthread_mutex_t*);
int pthread_mutex_timedlock(pthread_mutex_t*, struct timespec const* absolute_time);
int pthread_mutex_unlock(pthread_mutex_t*);
int pthread_mutexattr_init(pthread_mutexattr_t*);
int pthread_mutexattr_destroy(pthread_mutexattr_t*);
int pthread_mutexattr_gettype(pthread_mutexattr_t const*, int*);
int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
int pthread_mutexattr_getpshared(pthread_mutexattr_t const*, int*);
int pthread_mutexattr_setpshared(pthread_mutexattr_t*, int);

// Condition variables.
int pthread_cond_init(pthread_cond_t*, pthread_condattr_t const*);
int pthread_cond_destroy(pthread_cond_t*);
int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, struct timespec const* absolute_time);
int pthread_cond_signal(pthread_cond_t*);
int pthread_cond_broadcast(pthread_cond_t*);
int pthread_condattr_init(pthread_condattr_t*);
int pthread_condattr_destroy(pthread_condattr_t*);
int pthread_condattr_getclock(pthread_condattr_t const*, clockid_t*);
int pthread_condattr_setclock(pthread_condattr_t*, clockid_t);

// Read-write locks: readers go ahead of waiting writers, so a thread may
// always take a read lock it already holds.
int pthread_rwlock_init(pthread_rwlock_t*, pthread_rwlockattr_t const*);
int pthread_rwlock_destroy(pthread_rwlock_t*);
int pthread_rwlock_rdlock(pthread_rwlock_t*);
int pthread_rwlock_tryrdlock(pthread_rwlock_t*);
int pthread_rwlock_wrlock(pthread_rwlock_t*);
int pthread_rwlock_trywrlock(pthread_rwlock_t*);
int pthread_rwlock_unlock(pthread_rwlock_t*);
int pthread_rwlockattr_init(pthread_rwlockattr_t*);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t*);

// Barriers and spin locks.
int pthread_barrier_init(pthread_barrier_t*, pthread_barrierattr_t const*, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t*);
int pthread_barrier_wait(pthread_barrier_t*);
int pthread_barrierattr_init(pthread_barrierattr_t*);
int pthread_barrierattr_destroy(pthread_barrierattr_t*);
int pthread_spin_init(pthread_spinlock_t*, int shared);
int pthread_spin_destroy(pthread_spinlock_t*);
int pthread_spin_lock(pthread_spinlock_t*);
int pthread_spin_trylock(pthread_spinlock_t*);
int pthread_spin_unlock(pthread_spinlock_t*);

// Thread-specific data.
int pthread_key_create(pthread_key_t*, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t);
void* pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, void const* value);

// Cleanup handlers: run by pthread_exit(), or by pop with a non-zero argument.
struct __pthread_handler_rec {
    void (*routine)(void*);
    void* argument;
    struct __pthread_handler_rec* next;
};

void __pthread_cleanup_push(struct __pthread_handler_rec*);
void __pthread_cleanup_pop(int execute);

#define pthread_cleanup_push(handler, argument)                       \
    {                                                                 \
        struct __pthread_handler_rec __handler = { handler, argument, 0 }; \
        __pthread_cleanup_push(&__handler);

#define pthread_cleanup_pop(execute)   \
    __pthread_cleanup_pop(execute);    \
    }

__END_DECLS
