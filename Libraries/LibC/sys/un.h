/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/socket.h>

#define sockaddr_un vulture_sockaddr_un
#define SUN_LEN(address) (offsetof(struct sockaddr_un, sun_path) + strlen((address)->sun_path))

// For getsockopt on one, at level SOL_LOCAL.
#define SOL_LOCAL 0
#define LOCAL_PEERCRED 1
#include <sys/ucred.h>
