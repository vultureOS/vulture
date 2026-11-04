/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/socket.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define iovec vulture_iovec
#define IOV_MAX 1024

ssize_t readv(int fd, struct iovec const*, int count);
ssize_t writev(int fd, struct iovec const*, int count);

__END_DECLS
