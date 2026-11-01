/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

// Calls that exist only on vultureOS.

__BEGIN_DECLS

// Turns the machine off. Only init (process 1) may.
int vulture_power_off(void);

// A file with no name, of `size` bytes, for mmap(MAP_SHARED) in several
// processes (send the descriptor through a port). Flags: O_CLOEXEC.
int vulture_shared_memory_create(unsigned long size, int flags);

__END_DECLS
