/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <locale.h>
#include <string.h>

extern "C" {

char* setlocale(int, char const* locale)
{
    static char name[] = "C";
    static char utf8[] = "C.UTF-8";
    if (!locale || !*locale || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return name;
    if (strstr(locale, "UTF-8") || strstr(locale, "utf8"))
        return utf8;
    return nullptr;
}

struct lconv* localeconv(void)
{
    static char point[] = ".";
    static char empty[] = "";
    static struct lconv conventions = { point, empty, empty };
    return &conventions;
}
}
