/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <sys/sysctl.h>
#include <time.h>

// uptime: the time, how long since boot, and how many users.
int main()
{
    unsigned long long uptime_ns = 0;
    size_t length = sizeof(uptime_ns);
    sysctlbyname("kern.uptime_ns", &uptime_ns, &length, nullptr, 0);
    long long seconds = static_cast<long long>(uptime_ns / 1'000'000'000);
    time_t now = time(nullptr);
    struct tm broken;
    localtime_r(&now, &broken);
    char clock[16];
    strftime(clock, sizeof(clock), "%k:%M", &broken);
    printf("%s  up ", clock);
    long long days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;
    if (days > 0)
        printf("%lld day%s, ", days, days == 1 ? "" : "s");
    if (hours > 0)
        printf("%lld:%02lld, ", hours, minutes);
    else if (minutes > 0)
        printf("%lld min%s, ", minutes, minutes == 1 ? "" : "s");
    else
        printf("%lld sec%s, ", seconds, seconds == 1 ? "" : "s");
    // Users: sessions with a terminal.
    size_t list_length = 0;
    int users = 0;
    if (sysctlbyname("kern.proc.all", nullptr, &list_length, nullptr, 0) == 0) {
        list_length += 8 * sizeof(vulture_process_info);
        auto* processes = static_cast<vulture_process_info*>(malloc(list_length));
        if (sysctlbyname("kern.proc.all", processes, &list_length, nullptr, 0) == 0) {
            for (size_t i = 0; i < list_length / sizeof(vulture_process_info); ++i) {
                if (processes[i].pid == processes[i].sid && processes[i].terminal && processes[i].state != 'Z')
                    ++users;
            }
        }
        free(processes);
    }
    double load[3] = {};
    (void)getloadavg(load, 3);
    printf("%d user%s, load averages: %.2f %.2f %.2f\n", users, users == 1 ? "" : "s", load[0], load[1], load[2]);
    return 0;
}
