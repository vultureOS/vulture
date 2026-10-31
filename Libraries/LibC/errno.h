/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/errno.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// One per process for now; per thread once processes have several threads.
extern int errno;

__END_DECLS
