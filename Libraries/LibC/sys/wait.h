/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/wait.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

pid_t waitpid(pid_t, int* status, int options);
pid_t wait(int* status);

__END_DECLS
