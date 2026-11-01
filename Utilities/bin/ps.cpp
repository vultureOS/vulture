/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

// ps [-Aaefjlux] [-p pid]: processes, as macOS lays them out.
//   default  this terminal's processes: PID TTY TIME CMD
//   -A / -e  everyone's;  -u / aux  user-oriented;  -j  job-control columns
//   -f full (UID PID PPID ...)
namespace {

vulture_process_info* load(size_t& count)
{
    size_t length = 0;
    if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) < 0)
        return nullptr;
    length += 16 * sizeof(vulture_process_info);
    auto* processes = static_cast<vulture_process_info*>(malloc(length));
    if (sysctlbyname("kern.proc.all", processes, &length, nullptr, 0) < 0)
        return nullptr;
    count = length / sizeof(vulture_process_info);
    return processes;
}

void terminal_name(unsigned long long device, char* text, size_t size)
{
    unsigned major = static_cast<unsigned>(device >> 8), minor = static_cast<unsigned>(device & 0xff);
    if (device == 0)
        snprintf(text, size, "??");
    else if (major == 5 && minor == 1)
        snprintf(text, size, "console");
    else if (major == 16)
        snprintf(text, size, "ttys%03u", minor);
    else
        snprintf(text, size, "%u,%u", major, minor);
}

void cpu_time(unsigned long long nanoseconds, char* text, size_t size)
{
    unsigned long long hundredths = nanoseconds / 10'000'000;
    snprintf(text, size, "%llu:%02llu.%02llu", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
}

char const* user_name(unsigned uid)
{
    static char number[16];
    if (struct passwd* user = getpwuid(uid))
        return user->pw_name;
    snprintf(number, sizeof(number), "%u", uid);
    return number;
}

}

int main(int argc, char** argv)
{
    bool everyone = false, user_format = false, jobs_format = false, full_format = false, long_format = false;
    int only_pid = 0;
    for (int i = 1; i < argc; ++i) {
        char const* option = argv[i];
        if (strcmp(option, "-p") == 0 && i + 1 < argc) {
            only_pid = atoi(argv[++i]);
            continue;
        }
        for (char const* c = option[0] == '-' ? option + 1 : option; *c; ++c) {
            switch (*c) {
            case 'A':
            case 'e':
            case 'a':
            case 'x':
                everyone = true;
                break;
            case 'u':
                user_format = true;
                break;
            case 'j':
                jobs_format = true;
                break;
            case 'f':
                full_format = true;
                break;
            case 'l':
                long_format = true;
                break;
            default:
                fprintf(stderr, "usage: ps [-AaefjlUux] [-p pid]\n");
                return 1;
            }
        }
    }
    size_t count = 0;
    auto* processes = load(count);
    if (!processes) {
        perror("ps");
        return 1;
    }
    qsort(processes, count, sizeof(vulture_process_info), [](void const* a, void const* b) {
        return static_cast<vulture_process_info const*>(a)->pid - static_cast<vulture_process_info const*>(b)->pid;
    });
    // Without -A: processes on our terminal, owned by us.
    struct stat terminal {};
    bool have_terminal = fstat(STDIN_FILENO, &terminal) == 0 && S_ISCHR(terminal.st_mode) && isatty(STDIN_FILENO);
    uid_t me = getuid();
    unsigned long long memory = 0;
    size_t length = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &length, nullptr, 0);

    if (user_format)
        printf("USER               PID  %%CPU %%MEM      VSZ    RSS   TT  STAT STARTED      TIME COMMAND\n");
    else if (jobs_format)
        printf("USER    PID  PPID  PGID   SESS JOBC STAT   TT       TIME COMMAND\n");
    else if (full_format || long_format)
        printf("  UID   PID  PPID   C STIME   TTY           TIME CMD\n");
    else
        printf("  PID TTY           TIME CMD\n");
    for (size_t i = 0; i < count; ++i) {
        auto const& process = processes[i];
        if (only_pid && process.pid != only_pid)
            continue;
        if (!everyone && !only_pid) {
            if (process.uid != me)
                continue;
            if (have_terminal && process.terminal != terminal.st_rdev)
                continue;
        }
        char tty[16], time_text[32];
        terminal_name(process.terminal, tty, sizeof(tty));
        cpu_time(process.cpu_nanoseconds, time_text, sizeof(time_text));
        char state[4] = { process.state, process.pid == process.sid ? 's' : '\0', '\0', '\0' };
        if (user_format) {
            time_t started = process.start_time;
            struct tm broken;
            localtime_r(&started, &broken);
            char start_text[16];
            strftime(start_text, sizeof(start_text), "%l:%M%p", &broken);
            double memory_percent = memory ? 100.0 * static_cast<double>(process.resident_bytes) / static_cast<double>(memory) : 0;
            printf("%-15s %6d %5.1f %4.1f %8llu %6llu %4s  %-4s %7s %9s %s\n", user_name(process.uid), process.pid, 0.0, memory_percent,
                process.resident_bytes / 1024, process.resident_bytes / 1024, process.terminal ? tty + (strncmp(tty, "ttys", 4) == 0 ? 4 : 0) : "??",
                state, start_text, time_text, process.name);
        } else if (jobs_format) {
            printf("%-8s %5d %5d %5d %6d %4d %-4s %5s %10s %s\n", user_name(process.uid), process.pid, process.ppid, process.pgid, process.sid, 0, state,
                tty, time_text, process.name);
        } else if (full_format || long_format) {
            time_t started = process.start_time;
            struct tm broken;
            localtime_r(&started, &broken);
            char start_text[16];
            strftime(start_text, sizeof(start_text), "%l:%M%p", &broken);
            printf("%5u %5d %5d %3d %7s %-8s %10s %s\n", process.uid, process.pid, process.ppid, 0, start_text, tty, time_text, process.name);
        } else {
            printf("%5d %-8s %10s %s\n", process.pid, tty, time_text, process.name);
        }
    }
    free(processes);
    return 0;
}
