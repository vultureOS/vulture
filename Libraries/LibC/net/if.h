/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/socket.h>

#define IFNAMSIZ VULTURE_IFNAMSIZ
#define IF_NAMESIZE VULTURE_IFNAMSIZ
#define IFF_UP VULTURE_IFF_UP
#define IFF_BROADCAST VULTURE_IFF_BROADCAST
#define IFF_LOOPBACK VULTURE_IFF_LOOPBACK
#define IFF_RUNNING VULTURE_IFF_RUNNING
#define IFF_MULTICAST VULTURE_IFF_MULTICAST

__BEGIN_DECLS

unsigned int if_nametoindex(char const* name);
char* if_indextoname(unsigned int index, char* name);

__END_DECLS
