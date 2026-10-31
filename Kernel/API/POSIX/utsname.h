/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// uname(), shared by the kernel and LibC.

#define VULTURE_UTSNAME_LENGTH 65

struct vulture_utsname {
    char sysname[VULTURE_UTSNAME_LENGTH];
    char nodename[VULTURE_UTSNAME_LENGTH];
    char release[VULTURE_UTSNAME_LENGTH];
    char version[VULTURE_UTSNAME_LENGTH];
    char machine[VULTURE_UTSNAME_LENGTH];
};

// reboot()
#define VULTURE_REBOOT_RESTART 0
#define VULTURE_REBOOT_POWER_OFF 1
