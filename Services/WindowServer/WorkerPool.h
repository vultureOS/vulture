/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>
#include <pthread.h>

namespace WindowServer {

// Threads that take pieces of one job at a time: run() calls the job with each
// index below `count`, on whichever is free, the caller's thread among them, and
// returns when all are done. Only one thread calls run() at a time.
//
// A frame is many jobs in quick succession, and waking a sleeping thread takes
// an interprocessor interrupt, which is slow (more so under a hypervisor). So
// between jobs the workers spin a little before they sleep.
class WorkerPool {
public:
    using Job = void (*)(void* context, size_t index);

    explicit WorkerPool(int threads);

    void run(size_t count, Job, void* context);
    [[nodiscard]] int thread_count() const { return m_thread_count + 1; }

private:
    static void* worker_main(void*);
    void take_work();

    int m_thread_count { 0 };
    pthread_mutex_t m_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t m_start = PTHREAD_COND_INITIALIZER;
    u64 m_generation { 0 };
    int m_sleepers { 0 };

    Job m_job { nullptr };
    void* m_context { nullptr };
    size_t m_count { 0 };
    // The next index to hand out. Beyond any count between jobs, so nothing is
    // taken while the next job is being set up.
    size_t m_next { ~static_cast<size_t>(0) / 2 };
    size_t m_completed { 0 };
};

}
