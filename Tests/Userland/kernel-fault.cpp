/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <sys/sysctl.h>

// Asks the kernel to make a mistake with this program's memory: see Security/DebugFault.h and
// Meta/hardening-test.sh. The machine should not get past this.

int main()
{
    fflush(stdout);
    if (sysctlbyname("debug.fault", nullptr, nullptr, reinterpret_cast<void const*>(&main), 1) != 0) {
        perror("kernel-fault");
        return 1;
    }
    printf("kernel-fault: the kernel got away with it\n");
    return 1;
}
