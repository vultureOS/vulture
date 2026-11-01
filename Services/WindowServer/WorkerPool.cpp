/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WorkerPool.h"
#include <LibBase/StdLibExtras.h>
#include <sched.h>

namespace WindowServer {

namespace {

constexpr size_t no_work = ~static_cast<size_t>(0) / 2;
// About 200 microseconds, on what this runs on.
constexpr int spin_iterations = 20000;

void pause_processor()
{
    asm volatile("yield");
}

}

WorkerPool::WorkerPool(int threads)
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < threads - 1; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, &attributes, worker_main, this) == 0)
            ++m_thread_count;
    }
    pthread_attr_destroy(&attributes);
}

void* WorkerPool::worker_main(void* argument)
{
    auto* pool = static_cast<WorkerPool*>(argument);
    pthread_setname_np("compositor");
    u64 seen = 0;
    for (;;) {
        bool changed = false;
        for (int spin = 0; spin < spin_iterations; ++spin) {
            if (__atomic_load_n(&pool->m_generation, __ATOMIC_ACQUIRE) != seen) {
                changed = true;
                break;
            }
            pause_processor();
        }
        if (!changed) {
            pthread_mutex_lock(&pool->m_mutex);
            // Counted before looking: run() counts sleepers after publishing,
            // so one of the two sees the other.
            __atomic_add_fetch(&pool->m_sleepers, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&pool->m_generation, __ATOMIC_SEQ_CST) == seen)
                pthread_cond_wait(&pool->m_start, &pool->m_mutex);
            __atomic_sub_fetch(&pool->m_sleepers, 1, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&pool->m_mutex);
        }
        seen = __atomic_load_n(&pool->m_generation, __ATOMIC_ACQUIRE);
        pool->take_work();
    }
}

void WorkerPool::take_work()
{
    for (;;) {
        size_t index = __atomic_fetch_add(&m_next, 1, __ATOMIC_ACQ_REL);
        if (index >= __atomic_load_n(&m_count, __ATOMIC_ACQUIRE))
            return;
        // Having an index means the job cannot end, nor the next be set up,
        // until this one is counted as done.
        m_job(m_context, index);
        __atomic_add_fetch(&m_completed, 1, __ATOMIC_ACQ_REL);
    }
}

void WorkerPool::run(size_t count, Job job, void* context)
{
    if (count == 0)
        return;
    // Too little to be worth anyone's while.
    if (count == 1 || m_thread_count == 0) {
        for (size_t i = 0; i < count; ++i)
            job(context, i);
        return;
    }
    __atomic_store_n(&m_next, no_work, __ATOMIC_SEQ_CST);
    m_job = job;
    m_context = context;
    __atomic_store_n(&m_count, count, __ATOMIC_RELEASE);
    __atomic_store_n(&m_completed, static_cast<size_t>(0), __ATOMIC_RELEASE);
    __atomic_store_n(&m_next, static_cast<size_t>(0), __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&m_generation, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m_sleepers, __ATOMIC_SEQ_CST) > 0) {
        pthread_mutex_lock(&m_mutex);
        pthread_cond_broadcast(&m_start);
        pthread_mutex_unlock(&m_mutex);
    }
    take_work();
    int spins = 0;
    while (__atomic_load_n(&m_completed, __ATOMIC_ACQUIRE) != count) {
        pause_processor();
        // A worker with a piece was preempted: let it finish.
        if (++spins % 4096 == 0)
            sched_yield();
    }
    __atomic_store_n(&m_count, static_cast<size_t>(0), __ATOMIC_RELEASE);
}

}
