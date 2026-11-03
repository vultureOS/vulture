/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Job.h"
#include <BootstrapEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Listener.h>

namespace Launchd {

// Starts the jobs, watches them end, starts them again as their KeepAlive
// asks (no more often than ThrottleInterval allows), runs StartInterval
// jobs on time, and stops everything for shutdown. As process 1 it also
// reaps every orphan.
//
// It is the bootstrap (name) server of every process it starts: their
// bootstrap port is its. A job's MachServices exist from the moment it is
// loaded; vlaunchd keeps a share of each one's receive right, so messages
// sent while the job is not running wait, and start it.
class Supervisor final : public BootstrapStub {
public:
    explicit Supervisor(Core::EventLoop&);

    // Every *.plist in `directory`, in name order.
    // Jobs for the graphical session load only in one. Before loading any.
    void set_graphical_session(bool graphical) { m_graphical_session = graphical; }
    void load_directory(char const* directory);
    void load_file(char const* path);
    [[nodiscard]] size_t job_count() const { return m_jobs.size(); }

    // RunAtLoad and KeepAlive jobs now; StartInterval ones on time.
    void start();

    // Stops every job (SIGTERM, then SIGKILL after its ExitTimeOut), then
    // calls `then`.
    void stop_everything(Function<void()> then);

    // The bootstrap endpoint.
    BootstrapEndpoint::LookUpAnswer look_up(IPC::Sender const&, String name) override;
    BootstrapEndpoint::CheckInAnswer check_in(IPC::Sender const&, String name) override;
    i32 register_service(IPC::Sender const&, String name, IPC::Port service) override;

private:
    bool m_graphical_session { false };
    Vector<String> m_disabled_labels;
    int m_directories_loaded { 0 };
    struct Service {
        String name;
        IPC::Port receive; // Our share, for MachServices (empty when registered).
        IPC::Port send;
        Job* job { nullptr };
        int watch { 0 };
    };

    Service* find_service(StringView name);
    // Starts the service's job when a message waits for it.
    void watch_for_demand(Service&);
    void stop_watching(Job&);

    void spawn(Job&);
    void reap();
    void job_ended(Job&, int status);
    void check_stopped();

    Core::EventLoop& m_loop;
    Vector<OwnPtr<Job>> m_jobs;
    Vector<OwnPtr<Service>> m_services;
    OwnPtr<IPC::Listener> m_bootstrap;
    bool m_stopping { false };
    Function<void()> m_after_stopping;
};

}
