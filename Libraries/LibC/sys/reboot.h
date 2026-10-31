/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/utsname.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

#define RB_AUTOBOOT VULTURE_REBOOT_RESTART
#define RB_POWEROFF VULTURE_REBOOT_POWER_OFF
#define RB_HALT VULTURE_REBOOT_POWER_OFF

// Writes everything to disk, then restarts or powers off (root only).
int reboot(int how);

__END_DECLS
