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
#include <stdlib.h>
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
//   vlaunchd [--log FILE] [--gui] --directory DIR...
//
// (A job in an earlier directory is used in place of one of the same name in a later one, or, if it says "Disabled",
// keeps that one from running: how a test changes a few of the system's jobs and runs the rest as they are.)

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
        } else if (strcmp(argv[i], "--gui") == 0) {
            supervisor.set_graphical_session(true);
        } else if (strcmp(argv[i], "--directory") == 0 && i + 1 < argc) {
            supervisor.load_directory(argv[++i]);
        } else {
            fprintf(stderr, "usage: vlaunchd [--log FILE] [--gui] --directory DIR...\n");
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
    // From the kernel's "debug" flag: last on the line, whatever else there is.
    if (argc > 1 && strcmp(argv[argc - 1], "--debug") == 0) {
        Launchd::set_debug_logging(true);
        // Every job gets it, and what they start: logd and programs ask for it.
        setenv("VULTURE_DEBUG", "1", 1);
        --argc;
    }
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_tests(argc > 2 && strcmp(argv[2], "--debug") != 0 ? argv[2] : nullptr);
    }
    if (argc > 1 && strcmp(argv[1], "--crash-test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_storage_test(false);
    }
    if (argc > 1 && strcmp(argv[1], "--persist-test") == 0) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        return Launchd::run_storage_test(true);
    }
    bool ui_test = argc > 1 && strcmp(argv[1], "--ui-test") == 0;
    bool ui_gallery = argc > 1 && strcmp(argv[1], "--ui-gallery") == 0;
    bool ui_desktop = argc > 1 && strcmp(argv[1], "--ui-desktop") == 0;
    bool ui_budgets = argc > 1 && strcmp(argv[1], "--ui-budgets") == 0;
    bool sound_test = argc > 1 && strcmp(argv[1], "--sound-test") == 0;
    bool network_test = argc > 1 && strcmp(argv[1], "--network-test") == 0;
    bool network_gui_test = argc > 1 && strcmp(argv[1], "--network-gui-test") == 0;
    if (ui_test || ui_gallery || ui_desktop || ui_budgets || sound_test || network_test || network_gui_test) {
        printf("vlaunchd: vultureOS userland is up (process %d)\n", getpid());
        fflush(stdout);
        // The test's jobs alone, in a supervisor of their own: this process
        // only reaps, until the machine is switched off from outside.
        pid_t supervisor = fork();
        if (supervisor == 0) {
            // The desktop's walkthroughs are of the real system: everything it runs, with only what they must change.
            if (ui_desktop || ui_budgets)
                execl("/sbin/vlaunchd", "vlaunchd", "--gui", "--directory", ui_budgets ? "/usr/tests/budget-jobs" : "/usr/tests/desktop", "--directory", "/System/Library/LaunchDaemons", "--directory", "/Library/LaunchDaemons", nullptr);
            else
                execl("/sbin/vlaunchd", "vlaunchd", "--directory", network_gui_test ? "/usr/tests/network-gui" : network_test ? "/usr/tests/network" : sound_test ? "/usr/tests/sound" : (ui_gallery ? "/usr/tests/gallery" : "/usr/tests/ui"), nullptr);
            _exit(127);
        }
        // Nobody to stop here: asked to shut down (see tool_reboot), it just happens.
        signal(SIGUSR2, [](int) {
            sync();
            reboot(RB_POWEROFF);
        });
        signal(SIGINT, [](int) {
            sync();
            reboot(RB_AUTOBOOT);
        });
        for (;;)
            (void)waitpid(-1, nullptr, 0);
    }
    bool graphical = argc > 1 && strcmp(argv[1], "--gui") == 0;
    // Nothing to draw on: the text console is all there can be.
    if (graphical && access("/dev/gpu0", F_OK) != 0) {
        printf("vlaunchd: no display for a desktop: staying on the text console\n");
        graphical = false;
    }
    return run_as_process_one(graphical);
}
