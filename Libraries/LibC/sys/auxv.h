/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

#define AT_HWCAP 16

// AT_HWCAP alone, for now: what the processor can do.
unsigned long getauxval(unsigned long type);

__END_DECLS
