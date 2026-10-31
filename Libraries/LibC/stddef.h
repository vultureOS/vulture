/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#ifndef __cplusplus
typedef __WCHAR_TYPE__ wchar_t;
#endif
typedef struct {
    long long __ll __attribute__((aligned(__alignof__(long long))));
    long double __ld __attribute__((aligned(__alignof__(long double))));
} max_align_t;

#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)
