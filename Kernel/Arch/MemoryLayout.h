/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(__aarch64__)
#include <Kernel/Arch/aarch64/MemoryLayout.h>
#else
#error "Unsupported architecture"
#endif
