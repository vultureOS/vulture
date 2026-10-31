/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// System information by name, as on macOS: "kern.ostype", "kern.hostname",
// "kern.boottime", "hw.ncpu", "hw.memsize", "hw.machine"...
int sysctlbyname(char const* name, void* old_value, size_t* old_length, void const* new_value, size_t new_length);

__END_DECLS
