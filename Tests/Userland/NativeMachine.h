/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What uname(1) and the firmware's boot path are called on what's being tested.
#if defined(__x86_64__)
#    define NATIVE_MACHINE "x86_64"
#    define NATIVE_LOADER_LOWER "bootx64.efi"
#else
#    define NATIVE_MACHINE "arm64"
#    define NATIVE_LOADER_LOWER "bootaa64.efi"
#endif
