/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <unistd.h>

// killall [-dlmsv] [-SIGNAL] [-u user] name...: signals every process with that name that belongs to you.

namespace {

struct SignalName {
    char const* name;
    int number;
};

SignalName const signal_names[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "TRAP", SIGTRAP }, { "ABRT", SIGABRT }, { "FPE", SIGFPE },
    { "KILL", SIGKILL }, { "BUS", SIGBUS }, { "SEGV", SIGSEGV }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "URG", SIGURG },
    { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "CONT", SIGCONT }, { "CHLD", SIGCHLD }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
    { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
};

int signal_called(char const* name)
{
    if (strncmp(name, "SIG", 3) == 0)
        name += 3;
    for (auto const& entry : signal_names) {
        if (strcasecmp(entry.name, name) == 0)
            return entry.number;
    }
    char* end = nullptr;
    long const number = strtol(name, &end, 10);
    return end && *end == 0 && number >= 0 ? static_cast<int>(number) : -1;
}

}

int main(int argc, char** argv)
{
    int signal_number = SIGTERM;
    bool dry_run = false, verbose = false, list = false;
    uid_t uid = getuid();
    bool everyone = getuid() == 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        char const* option = argv[first] + 1;
        if (strcmp(option, "l") == 0) {
            list = true;
        } else if (strcmp(option, "s") == 0 || strcmp(option, "d") == 0) {
            dry_run = true;
            verbose = true;
        } else if (strcmp(option, "v") == 0) {
            verbose = true;
        } else if (strcmp(option, "u") == 0 && first + 1 < argc) {
            auto const* user = getpwnam(argv[++first]);
            if (!user) {
                fprintf(stderr, "No user named %s\n", argv[first]);
                return 1;
            }
            uid = user->pw_uid;
            everyone = false;
        } else {
            int const number = signal_called(option);
            if (number < 0) {
                fprintf(stderr, "killall: Unknown signal %s\n", option);
                return 1;
            }
            signal_number = number;
        }
    }
    if (list) {
        for (auto const& entry : signal_names)
            printf("%s ", entry.name);
        printf("\n");
        return 0;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: killall [-dlsv] [-SIGNAL] [-u user] name ...\n");
        return 1;
    }
    size_t length = 0;
    if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) < 0) {
        warn("killall", "kern.proc.all");
        return 1;
    }
    auto* processes = static_cast<vulture_process_info*>(malloc(length));
    if (!processes || sysctlbyname("kern.proc.all", processes, &length, nullptr, 0) < 0) {
        warn("killall", "kern.proc.all");
        return 1;
    }
    size_t const count = length / sizeof(vulture_process_info);
    int matched = 0;
    pid_t const self = getpid();
    for (size_t i = 0; i < count; ++i) {
        auto const& process = processes[i];
        if (process.pid == self || (!everyone && static_cast<uid_t>(process.uid) != uid))
            continue;
        bool wanted = false;
        for (int name = first; name < argc; ++name)
            wanted = wanted || strcmp(process.name, argv[name]) == 0;
        if (!wanted)
            continue;
        ++matched;
        if (verbose)
            printf("kill -%d %d\n", signal_number, process.pid);
        if (!dry_run && kill(process.pid, signal_number) != 0)
            warn("killall", process.name);
    }
    free(processes);
    if (matched == 0) {
        fprintf(stderr, "No matching processes %swere found\n", everyone ? "" : "belonging to you ");
        return 1;
    }
    return 0;
}
