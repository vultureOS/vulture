/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the dynamic linker does for the C library once a program's running: it's the one that knows what's loaded. It hands the
// C library this table before any initializer runs. A program linked statically never gets one, as there's no linker to ask.

namespace System {

// The numbers in dlfcn.h, for dyld, which has no C library's headers to use.
constexpr int dyld_mode_local = 0x4;
constexpr int dyld_mode_global = 0x8;
constexpr int dyld_mode_no_load = 0x10;
constexpr int dyld_mode_no_delete = 0x80;

constexpr long dyld_handle_next = -1;
constexpr long dyld_handle_default = -2;
constexpr long dyld_handle_self = -3;
constexpr long dyld_handle_main_only = -5;

struct DyldServices {
    void* (*open)(char const* path, int mode);
    int (*close)(void* handle);
    void* (*symbol)(void* handle, char const* name, void const* caller);
    int (*address)(void const* address, char const** file, void** base, char const** symbol_name, void** symbol_address);
    // What the last one that failed said, once.
    char const* (*take_error)();
};

}
