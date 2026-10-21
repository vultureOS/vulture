/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Fixed-width types built from compiler-provided macros, so nothing here
// depends on system headers.

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using i8 = __INT8_TYPE__;
using i16 = __INT16_TYPE__;
using i32 = __INT32_TYPE__;
using i64 = __INT64_TYPE__;

using size_t = __SIZE_TYPE__;
using ssize_t = __PTRDIFF_TYPE__;
using ptrdiff_t = __PTRDIFF_TYPE__;
using uintptr_t = __UINTPTR_TYPE__;
using intptr_t = __INTPTR_TYPE__;
using nullptr_t = decltype(nullptr);

// An address held as an integer.
using FlatPtr = __UINTPTR_TYPE__;

static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
static_assert(sizeof(size_t) == sizeof(void*));

constexpr u64 KiB = 1024;
constexpr u64 MiB = 1024 * KiB;
constexpr u64 GiB = 1024 * MiB;
