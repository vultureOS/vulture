/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

#define MNT_RDONLY 0x00000001
#define MNT_FORCE 0x00080000

// Of `type`, "msdos" and the like, from this block device (`data`), at this directory.
int mount(char const* type, char const* directory, int flags, void* data);
int unmount(char const* directory, int flags);

__END_DECLS
