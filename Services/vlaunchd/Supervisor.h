/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Job.h"
#include <LibCore/EventLoop.h>

namespace Launchd {

// Starts the jobs, watches them end, starts them again as their KeepAlive
// asks (no more often than ThrottleInterval allows), runs StartInterval
// jobs on time, and stops everything for shutdown. As process 1 it also
// reaps every orphan.
class Supervisor {
public:
    explicit Supervisor(Core::EventLoop&);

    // Every *.plist in `directory`, in name order.
    void load_directory(char const* directory);
    void load_file(char const* path);
    [[nodiscard]] size_t job_count() const { return m_jobs.size(); }

    // RunAtLoad and KeepAlive jobs now; StartInterval ones on time.
    void start();

    // Stops every job (SIGTERM, then SIGKILL after its ExitTimeOut), then
    // calls `then`.
    void stop_everything(Function<void()> then);

private:
    void spawn(Job&);
    void reap();
    void job_ended(Job&, int status);
    void check_stopped();

    Core::EventLoop& m_loop;
    Vector<OwnPtr<Job>> m_jobs;
    bool m_stopping { false };
    Function<void()> m_after_stopping;
};

}
