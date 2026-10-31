/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

#define FNM_NOMATCH 1
#define FNM_NOESCAPE 0x01
#define FNM_PATHNAME 0x02
#define FNM_PERIOD 0x04
#define FNM_CASEFOLD 0x10

// Shell wildcards: * ? [set] [!set] [a-z], with \ quoting.
int fnmatch(char const* pattern, char const* string, int flags);

__END_DECLS
