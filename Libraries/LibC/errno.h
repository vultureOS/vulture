/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/errno.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// Each thread has its own, as on macOS.
int* __error(void);
#define errno (*__error())

__END_DECLS
