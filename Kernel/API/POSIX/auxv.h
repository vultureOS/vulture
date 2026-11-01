/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The auxiliary vector: (type, value) pairs the kernel puts on a new
// program's stack after the environment, ending with AT_NULL. The dynamic
// linker finds the program through it.

#define AT_NULL 0
#define AT_PHDR 3   // The program's program headers, in memory.
#define AT_PHENT 4  // The size of one.
#define AT_PHNUM 5  // How many.
#define AT_PAGESZ 6
#define AT_BASE 7   // Where the interpreter (dynamic linker) was loaded.
#define AT_FLAGS 8
#define AT_ENTRY 9  // The program's entry point.
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_SECURE 23 // Set-user-ID or set-group-ID: trust no environment.
#define AT_EXECFN 31 // The path the program was run by.
