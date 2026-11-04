/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// mmap() protections and flags, shared by the kernel and LibC.

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_STACK 0x20000

#define VULTURE_MAP_FAILED ((void*)-1)

#define VULTURE_MADV_NORMAL 0
#define VULTURE_MADV_RANDOM 1
#define VULTURE_MADV_SEQUENTIAL 2
#define VULTURE_MADV_WILLNEED 3
#define VULTURE_MADV_DONTNEED 4
#define VULTURE_MADV_FREE 5
