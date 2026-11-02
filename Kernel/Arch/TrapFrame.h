/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// Arch-neutral code includes this, not an arch directory directly.
#if defined(__aarch64__)
#include <Kernel/Arch/aarch64/TrapFrame.h>
#elif defined(__x86_64__)
#include <Kernel/Arch/x86_64/TrapFrame.h>
#else
#error "Unsupported architecture"
#endif
