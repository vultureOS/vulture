/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

// Calls that exist only on vultureOS.

__BEGIN_DECLS

// Turns the machine off. Only init (process 1) may.
int vulture_power_off(void);

__END_DECLS
