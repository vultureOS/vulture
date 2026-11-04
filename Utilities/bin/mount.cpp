/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

// mount [-t type] [-r] device directory, and with nothing else what's mounted. umount directory.

int main(int argc, char** argv)
{
    char const* name = last_component(argv[0]);
    if (strcmp(name, "umount") == 0) {
        if (argc != 2) {
            fprintf(stderr, "usage: umount directory\n");
            return 1;
        }
        if (unmount(argv[1], 0) != 0) {
            fprintf(stderr, "umount: unmount(%s): %s\n", argv[1], strerror(errno));
            return 1;
        }
        return 0;
    }
    char const* type = "msdos";
    int flags = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-t") == 0 && first + 1 < argc)
            type = argv[++first];
        else if (strcmp(argv[first], "-r") == 0)
            flags |= MNT_RDONLY;
        else {
            fprintf(stderr, "usage: mount [-r] [-t type] device directory\n");
            return 1;
        }
    }
    if (first >= argc) {
        size_t size = 0;
        if (sysctlbyname("vfs.mounts", nullptr, &size, nullptr, 0) != 0) {
            warn("mount", "vfs.mounts");
            return 1;
        }
        auto* mounts = static_cast<vulture_mount_info*>(malloc(size));
        if (!mounts || sysctlbyname("vfs.mounts", mounts, &size, nullptr, 0) != 0) {
            warn("mount", "vfs.mounts");
            return 1;
        }
        for (size_t i = 0; i < size / sizeof(vulture_mount_info); ++i) {
            auto const& mount_point = mounts[i];
            printf("%s on %s (%s%s)\n", mount_point.device[0] ? mount_point.device : mount_point.type, mount_point.point, mount_point.type, mount_point.read_only ? ", read-only" : "");
        }
        return 0;
    }
    if (argc - first != 2) {
        fprintf(stderr, "usage: mount [-r] [-t type] device directory\n");
        return 1;
    }
    if (mount(type, argv[first + 1], flags, argv[first]) != 0) {
        fprintf(stderr, "mount: %s on %s: %s\n", argv[first], argv[first + 1], strerror(errno));
        return 1;
    }
    return 0;
}
