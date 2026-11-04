/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

// system_profiler [-listDataTypes] [SPSoftwareDataType] [SPHardwareDataType]: about this computer, as a Mac says it.

namespace {

void print_software()
{
    utsname name {};
    uname(&name);
    long long uptime_nanoseconds = 0;
    size_t size = sizeof(uptime_nanoseconds);
    (void)sysctlbyname("kern.uptime_ns", &uptime_nanoseconds, &size, nullptr, 0);
    long long const seconds = uptime_nanoseconds / 1'000'000'000;
    char const* user = getenv("USER");
    printf("Software:\n\n    System Software Overview:\n\n");
    printf("      System Version: vultureOS %s\n", name.release);
    printf("      Kernel Version: %s %s\n", name.sysname, name.release);
    printf("      Boot Volume: Vulture HD\n");
    printf("      Computer Name: %s\n", name.nodename);
    printf("      User Name: %s\n", user ? user : "unknown");
    printf("      Time since boot: %lld days, %lld hours, %lld minutes\n\n", seconds / 86400, seconds / 3600 % 24, seconds / 60 % 60);
}

void print_hardware()
{
    utsname name {};
    uname(&name);
    int processors = 1;
    size_t size = sizeof(processors);
    (void)sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0);
    unsigned long long memory = 0;
    size = sizeof(memory);
    (void)sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
    printf("Hardware:\n\n    Hardware Overview:\n\n");
    printf("      Machine: %s\n", name.machine);
    printf("      Total Number of Cores: %d\n", processors);
    printf("      Memory: %llu GB\n\n", memory / (1024ull * 1024 * 1024));
}

}

int main(int argc, char** argv)
{
    bool software = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-listDataTypes") == 0) {
            puts("Available Datatypes:\nSPHardwareDataType\nSPSoftwareDataType");
            return 0;
        }
        if (strcmp(argv[i], "SPSoftwareDataType") == 0)
            software = true;
        else if (strcmp(argv[i], "SPHardwareDataType") == 0)
            hardware = true;
        else if (strcmp(argv[i], "-detailLevel") == 0 && i + 1 < argc)
            ++i;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: system_profiler [-listDataTypes] [SPSoftwareDataType] [SPHardwareDataType]\n");
            return 1;
        }
    }
    if (!software && !hardware)
        software = hardware = true;
    if (hardware)
        print_hardware();
    if (software)
        print_software();
    return 0;
}
