/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

// Only the C (and UTF-8) locale, for now.
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define LC_MESSAGES 6

struct lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
};

char* setlocale(int category, char const* locale);
struct lconv* localeconv(void);

__END_DECLS
