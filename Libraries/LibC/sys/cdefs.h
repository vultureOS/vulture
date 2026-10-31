/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Lets the C library's headers serve C and C++ alike.
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#define __noreturn __attribute__((noreturn))
#define __printf_like(format_index, first_argument) __attribute__((format(printf, format_index, first_argument)))
