/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <netinet/in.h>

__BEGIN_DECLS

in_addr_t inet_addr(char const*);
int inet_aton(char const*, struct in_addr*);
char* inet_ntoa(struct in_addr);
int inet_pton(int family, char const* text, void* address);
char const* inet_ntop(int family, void const* address, char* buffer, socklen_t size);

__END_DECLS
