/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <sys/sysctl.h>

// df [-hk]: space on each mounted filesystem, as macOS lists it.
static void size_text(unsigned long long bytes, bool human, char* text, size_t length)
{
    if (!human) {
        snprintf(text, length, "%llu", bytes / 512);
        return;
    }
    char const units[] = "BKMGTP";
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 5) {
        value /= 1024;
        ++unit;
    }
    snprintf(text, length, unit == 0 ? "%.0f%c" : (value < 10 ? "%.1f%ci" : "%.0f%ci"), value, units[unit]);
}

int main(int argc, char** argv)
{
    bool human = false, kilobytes = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-H") == 0)
            human = true;
        else if (strcmp(argv[i], "-k") == 0)
            kilobytes = true;
    }
    size_t length = 0;
    if (sysctlbyname("vfs.mounts", nullptr, &length, nullptr, 0) < 0) {
        perror("df");
        return 1;
    }
    length += 8 * sizeof(vulture_mount_info);
    auto* mounts = static_cast<vulture_mount_info*>(malloc(length));
    if (sysctlbyname("vfs.mounts", mounts, &length, nullptr, 0) < 0) {
        perror("df");
        return 1;
    }
    size_t count = length / sizeof(vulture_mount_info);
    printf(human ? "Filesystem      Size   Used  Avail Capacity  Mounted on\n" : (kilobytes ? "Filesystem    1024-blocks       Used  Available Capacity  Mounted on\n" : "Filesystem     512-blocks       Used  Available Capacity  Mounted on\n"));
    for (size_t i = 0; i < count; ++i) {
        auto const& mount = mounts[i];
        unsigned long long total = mount.blocks * mount.block_size;
        unsigned long long available = mount.free_blocks * mount.block_size;
        unsigned long long used = total - available;
        int capacity = total ? static_cast<int>((used * 100 + total - 1) / total) : 0;
        char total_text[32], used_text[32], available_text[32];
        if (kilobytes && !human) {
            snprintf(total_text, sizeof(total_text), "%llu", total / 1024);
            snprintf(used_text, sizeof(used_text), "%llu", used / 1024);
            snprintf(available_text, sizeof(available_text), "%llu", available / 1024);
        } else {
            size_text(total, human, total_text, sizeof(total_text));
            size_text(used, human, used_text, sizeof(used_text));
            size_text(available, human, available_text, sizeof(available_text));
        }
        if (human)
            printf("%-14s %5s %6s %6s %7d%%   %s\n", mount.device, total_text, used_text, available_text, capacity, mount.point);
        else
            printf("%-14s %10s %10s %10s %7d%%   %s\n", mount.device, total_text, used_text, available_text, capacity, mount.point);
    }
    free(mounts);
    return 0;
}
