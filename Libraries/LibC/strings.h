/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

int strcasecmp(char const*, char const*);
int strncasecmp(char const*, char const*, size_t);
int ffs(int);

__END_DECLS
