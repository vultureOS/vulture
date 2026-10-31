/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdio.h>

// A failed CHECK prints where it failed and makes the program exit with 1
// (see finish()); init reports the program as failed.

inline int g_failures = 0;

#define CHECK(expression)                                                                       \
    do {                                                                                        \
        if (!(expression)) {                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE_NAME__, __LINE__, #expression); \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

inline int finish()
{
    return g_failures ? 1 : 0;
}
