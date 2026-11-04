/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/sysctl.h>
#include <unistd.h>

// purge: lets go of everything in the file cache that's already on the disk, as on a Mac.
int main()
{
    int one = 1;
    if (getuid() != 0) {
        fprintf(stderr, "Unable to purge disk buffers: Operation not permitted\n");
        return 1;
    }
    if (sysctlbyname("vm.purge", nullptr, nullptr, &one, sizeof(one)) != 0) {
        perror("purge");
        return 1;
    }
    return 0;
}
