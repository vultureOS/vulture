/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <LibDiskManagement/Disks.h>
#include <sys/sysctl.h>

// bless: which system the machine starts from, as on a Mac. --device /dev/diskNsM --setBoot chooses one, --getBoot says which
// it started from.

int main(int argc, char** argv)
{
    char const* device = nullptr;
    bool set_boot = false;
    bool get_boot = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "--setBoot") == 0)
            set_boot = true;
        else if (strcmp(argv[i], "--getBoot") == 0)
            get_boot = true;
    }
    if (get_boot) {
        vulture_mount_info mounts[32];
        size_t size = sizeof(mounts);
        if (sysctlbyname("vfs.mounts", mounts, &size, nullptr, 0) != 0) {
            perror("bless");
            return 1;
        }
        for (size_t i = 0; i < size / sizeof(mounts[0]); ++i) {
            if (strcmp(mounts[i].point, "/") == 0)
                printf("/dev/%s\n", mounts[i].device);
        }
        return 0;
    }
    if (!device || !set_boot) {
        fprintf(stderr, "Usage: bless --device /dev/diskNsM --setBoot\n       bless --getBoot\n");
        return 1;
    }
    StringBuilder line;
    line.appendff("set-startup {}", StringView { device, strlen(device) });
    auto const result = DiskManagement::request(line.view());
    if (result.is_error()) {
        fprintf(stderr, "bless: %s: %s\n", device, strerror(result.error().code()));
        return 1;
    }
    return 0;
}
