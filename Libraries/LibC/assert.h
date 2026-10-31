/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

__noreturn void __assertion_failed(char const* expression, char const* file, unsigned line, char const* function);

__END_DECLS

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __assertion_failed(#expression, __FILE_NAME__, __LINE__, __func__))
#endif
