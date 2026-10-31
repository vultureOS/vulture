/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/utsname.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct utsname {
    char sysname[VULTURE_UTSNAME_LENGTH];
    char nodename[VULTURE_UTSNAME_LENGTH];
    char release[VULTURE_UTSNAME_LENGTH];
    char version[VULTURE_UTSNAME_LENGTH];
    char machine[VULTURE_UTSNAME_LENGTH];
};

int uname(struct utsname*);

__END_DECLS
