/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Log.h"
#include "Supervisor.h"
#include "TestRunner.h"
#include <LibCore/EventLoop.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

// vlaunchd: process 1, as launchd is on macOS. It loads the jobs in
// /System/Library/LaunchDaemons and /Library/LaunchDaemons (JSON property lists, see Job.h) and keeps them
// as they ask: the console's getty, so there is a login, and the system
// services (logd, notifyd, configd, which names the machine) among them. It
// reaps every orphan. To shut down: SIGUSR2 or SIGTERM powers off, SIGINT
// restarts (what reboot, halt and shutdown send), each after stopping the
// jobs and writing everything to disk.
//
// Boot flags make it a test runner instead (see TestRunner.h).
//
// Not as process 1, it supervises the jobs in the directories it is given
// and stops them all when sent SIGTERM or SIGINT:
//
//   vlaunchd [--log FILE] --directory DIR...

namespace {

int run_as_process_one(bool graphical)
{
    printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
    fflush(stdout);
    Launchd::open_log("/var/log/system.log");

    Core::EventLoop loop;
    Launchd::Supervisor supervisor(loop);
    supervisor.set_graphical_session(graphical);
    supervisor.load_directory("/System/Library/LaunchDaemons");
    supervisor.load_directory("/Library/LaunchDaemons");
    supervisor.start();

    bool shutting_down = false;
    auto shut_down = [&](int how) {
        if (shutting_down)
            return;
        shutting_down = true;
        Launchd::log("%s requested", how == RB_AUTOBOOT ? "Restart" : "Shutdown");
        supervisor.stop_everything([how] {
            sync();
            printf("vlaunchd: %s\n", how == RB_AUTOBOOT ? "restarting" : "powering off");
            fflush(stdout);
            reboot(how);
        });
    };
    loop.on_signal(SIGUSR2, [&] { shut_down(RB_POWEROFF); });
    loop.on_signal(SIGTERM, [&] { shut_down(RB_POWEROFF); });
    loop.on_signal(SIGINT, [&] { shut_down(RB_AUTOBOOT); });
    // Terminals' hangups and the like are not for process 1.
    signal(SIGHUP, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    return loop.exec();
}

int run_as_supervisor(int argc, char** argv)
{
    Core::EventLoop loop;
    Launchd::Supervisor supervisor(loop);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            Launchd::open_log(argv[++i]);
        } else if (strcmp(argv[i], "--directory") == 0 && i + 1 < argc) {
            supervisor.load_directory(argv[++i]);
        } else {
            fprintf(stderr, "usage: vlaunchd [--log FILE] --directory DIR...\n");
            return 1;
        }
    }
    supervisor.start();
    auto stop = [&] { supervisor.stop_everything([&] { loop.quit(0); }); };
    loop.on_signal(SIGTERM, stop);
    loop.on_signal(SIGINT, stop);
    return loop.exec();
}

}

int main(int argc, char** argv)
{
    if (getpid() != 1)
        return run_as_supervisor(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_tests();
    }
    if (argc > 1 && strcmp(argv[1], "--crash-test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_storage_test(false);
    }
    if (argc > 1 && strcmp(argv[1], "--persist-test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_storage_test(true);
    }
    if (argc > 1 && strcmp(argv[1], "--ui-test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        fflush(stdout);
        // The test's jobs alone, in a supervisor of their own: this process
        // only reaps, until the machine is switched off from outside.
        pid_t supervisor = fork();
        if (supervisor == 0) {
            execl("/sbin/vlaunchd", "vlaunchd", "--directory", "/usr/tests/ui", nullptr);
            _exit(127);
        }
        for (;;)
            (void)waitpid(-1, nullptr, 0);
    }
    return run_as_process_one(argc > 1 && strcmp(argv[1], "--gui") == 0);
}
