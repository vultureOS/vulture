/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

// May change `path` and return a pointer into it (or a static string).
char* basename(char* path);
char* dirname(char* path);

__END_DECLS
