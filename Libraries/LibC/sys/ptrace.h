/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/ptrace.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

int ptrace(int request, pid_t pid, void* address, int data);

__END_DECLS
